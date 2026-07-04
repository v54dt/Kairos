#include "fill_model.h"

#include <algorithm>
#include <utility>

namespace kairos::exec {

namespace {

long DisplayedAt(const TopOfBook& b, Side side, Cents price) {
  if (side == Side::kBuy) {
    for (int i = 0; i < b.n_bids; ++i)
      if (b.bids[i].price == price) return b.bids[i].volume;
  } else {
    for (int i = 0; i < b.n_asks; ++i)
      if (b.asks[i].price == price) return b.asks[i].volume;
  }
  return 0;
}

// Drop consumed-liquidity entries for levels no longer displayed and clamp the rest to the current
// display, keying by ask/bid `side` (Side::kSell => ask levels, Side::kBuy => bid levels).
void PruneTaken(std::map<Cents, long>* taken, const TopOfBook& b, Side side) {
  for (auto it = taken->begin(); it != taken->end();) {
    long disp = DisplayedAt(b, side, it->first);
    if (disp <= 0) {
      it = taken->erase(it);
    } else {
      it->second = std::min(it->second, disp);
      ++it;
    }
  }
}

}  // namespace

SymbolFillModel::SymbolFillModel(std::string symbol, FillMode mode, SimAckFn ack, SimFillFn fill,
                                 SimCancelFn cancel)
    : symbol_(std::move(symbol)),
      mode_(mode),
      on_ack_(std::move(ack)),
      on_fill_(std::move(fill)),
      on_cancel_(std::move(cancel)) {}

void SymbolFillModel::EmitFill(Resting* r, long shares, Cents price) {
  if (shares <= 0) return;
  r->order.filled += shares;
  if (on_fill_) on_fill_(r->order.id, Fill{shares, price});
}

void SymbolFillModel::DropFilled() {
  resting_.erase(std::remove_if(resting_.begin(), resting_.end(),
                                [](const Resting& r) { return r.order.remaining() <= 0; }),
                 resting_.end());
}

void SymbolFillModel::MarketableWalk(SimOrder* order) {
  if (order->side == Side::kBuy) {
    for (int i = 0; i < book_.n_asks && order->remaining() > 0; ++i) {
      const Level& lvl = book_.asks[i];
      if (lvl.price > order->price) break;  // levels are ascending; nothing deeper crosses
      long take = std::min(order->remaining(), lvl.volume - ask_taken_[lvl.price]);
      if (take <= 0) continue;
      order->filled += take;
      ask_taken_[lvl.price] += take;
      if (on_fill_) on_fill_(order->id, Fill{take, lvl.price});
    }
  } else {
    for (int i = 0; i < book_.n_bids && order->remaining() > 0; ++i) {
      const Level& lvl = book_.bids[i];
      if (lvl.price < order->price) break;  // levels are descending
      long take = std::min(order->remaining(), lvl.volume - bid_taken_[lvl.price]);
      if (take <= 0) continue;
      order->filled += take;
      bid_taken_[lvl.price] += take;
      if (on_fill_) on_fill_(order->id, Fill{take, lvl.price});
    }
  }
}

// Fills the crossing (strictly-through) portion of a resting order against still-unconsumed
// displayed liquidity, marking it consumed so it is not re-filled on a later book. Returns shares
// taken.
long SymbolFillModel::ConsumeCrossing(const Resting& r) {
  long want = r.order.remaining();
  long got = 0;
  if (r.order.side == Side::kBuy) {
    for (int i = 0; i < book_.n_asks && want > 0; ++i) {
      const Level& lvl = book_.asks[i];
      if (lvl.price >= r.order.price) break;  // ascending; only strictly-through levels cross
      long take = std::min(want, lvl.volume - ask_taken_[lvl.price]);
      if (take <= 0) continue;
      ask_taken_[lvl.price] += take;
      want -= take;
      got += take;
    }
  } else {
    for (int i = 0; i < book_.n_bids && want > 0; ++i) {
      const Level& lvl = book_.bids[i];
      if (lvl.price <= r.order.price) break;  // descending; only strictly-through levels cross
      long take = std::min(want, lvl.volume - bid_taken_[lvl.price]);
      if (take <= 0) continue;
      bid_taken_[lvl.price] += take;
      want -= take;
      got += take;
    }
  }
  return got;
}

void SymbolFillModel::ReconcileTaken() {
  PruneTaken(&ask_taken_, book_, Side::kSell);
  PruneTaken(&bid_taken_, book_, Side::kBuy);
}

void SymbolFillModel::Submit(const SimOrder& order) {
  if (on_ack_) on_ack_(order.id, true, "");
  PlaceResting(order);
}

void SymbolFillModel::PlaceResting(const SimOrder& order) {
  SimOrder ord = order;
  if (book_.valid && !book_.is_trial) MarketableWalk(&ord);
  if (ord.remaining() <= 0) return;

  Resting r;
  r.order = ord;
  if (mode_ == FillMode::kProbQueue) {
    r.displayed_prev = DisplayedAt(book_, ord.side, ord.price);
    r.queue_ahead = r.displayed_prev;
    r.pending_trade_vol = 0;
  }
  resting_.push_back(std::move(r));
}

void SymbolFillModel::OnTrade(Cents price, long vol, std::int64_t, bool is_trial) {
  if (!matching_ || is_trial || vol <= 0) return;

  if (mode_ == FillMode::kConservative) {
    long budget = vol;  // shared across resting orders, time-priority order
    for (auto& r : resting_) {
      if (budget <= 0) break;
      bool through = r.order.side == Side::kBuy ? price < r.order.price : price > r.order.price;
      if (!through) continue;
      long f = std::min(r.order.remaining(), budget);
      EmitFill(&r, f, r.order.price);
      budget -= f;
    }
  } else {
    // Volume past the shared queue is a single budget: earlier same-price orders
    // consume it first (time priority), so stacked own orders never double-fill it.
    long spill_taken = 0;
    for (auto& r : resting_) {
      bool through = r.order.side == Side::kBuy ? price < r.order.price : price > r.order.price;
      if (through) {
        // Price traded strictly through the limit: the resting level is gone, the
        // order fills in full (matches hftbacktest's trade-through behavior).
        EmitFill(&r, r.order.remaining(), r.order.price);
        continue;
      }
      if (price != r.order.price) continue;
      long spill = std::max(0L, vol - r.queue_ahead);
      long f = std::min(r.order.remaining(), std::max(0L, spill - spill_taken));
      spill_taken += f;
      r.queue_ahead = std::max(0L, r.queue_ahead - vol);
      r.pending_trade_vol += vol;
      EmitFill(&r, f, r.order.price);
    }
  }
  DropFilled();
}

void SymbolFillModel::OnBook(const TopOfBook& book, std::int64_t) {
  book_ = book;
  if (!book.valid || book.is_trial) return;
  ReconcileTaken();
  if (!matching_) return;

  if (mode_ == FillMode::kConservative) {
    for (auto& r : resting_) {
      Cents best = r.order.side == Side::kBuy ? book.best_ask() : book.best_bid();
      bool through = r.order.side == Side::kBuy ? (best > 0 && best < r.order.price)
                                                : (best > 0 && best > r.order.price);
      if (!through) continue;
      EmitFill(&r, ConsumeCrossing(r), r.order.price);
    }
  } else {
    for (auto& r : resting_) {
      long now = DisplayedAt(book, r.order.side, r.order.price);
      long decrease = r.displayed_prev - now;
      if (decrease > 0) {
        long d_eff = std::max(0L, decrease - r.pending_trade_vol);
        r.pending_trade_vol = std::max(0L, r.pending_trade_vol - decrease);
        if (d_eff > 0 && r.displayed_prev > 0) {
          long advance = (d_eff * r.queue_ahead) / r.displayed_prev;  // floor
          r.queue_ahead = std::max(0L, r.queue_ahead - advance);
        }
      }
      r.displayed_prev = now;
    }
  }
  DropFilled();
}

bool SymbolFillModel::Cancel(const std::string& id, std::int64_t) {
  for (auto it = resting_.begin(); it != resting_.end(); ++it) {
    if (it->order.id == id) {
      resting_.erase(it);
      if (on_cancel_) on_cancel_(id, true);
      return true;
    }
  }
  return false;
}

void SymbolFillModel::ExpireAllResting() {
  for (const auto& r : resting_) {
    if (on_cancel_) on_cancel_(r.order.id, true);
  }
  resting_.clear();
}

}  // namespace kairos::exec
