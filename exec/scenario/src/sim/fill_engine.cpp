#include "fill_engine.h"

#include <utility>

namespace kairos::exec {

FillEngine::FillEngine(FillMode mode, SimAckFn ack, SimFillFn fill, SimCancelFn cancel)
    : mode_(mode),
      on_ack_(std::move(ack)),
      on_fill_(std::move(fill)),
      on_cancel_(std::move(cancel)) {}

void FillEngine::AddSymbol(const std::string& symbol) {
  if (symbols_.count(symbol)) return;
  SymbolState st;
  st.model = std::make_unique<SymbolFillModel>(
      symbol, mode_,
      [this](const std::string& id, bool ok, const std::string& e) {
        if (on_ack_) on_ack_(id, ok, e);
      },
      [this](const std::string& id, const Fill& f) { EmitFill(id, f); },
      [this](const std::string& id, bool ok) {
        if (ok) {
          id_symbol_.erase(id);
          id_remaining_.erase(id);
        }
        if (on_cancel_) on_cancel_(id, ok);
      });
  symbols_.emplace(symbol, std::move(st));
}

void FillEngine::EmitFill(const std::string& id, const Fill& f) {
  auto it = id_remaining_.find(id);
  if (it != id_remaining_.end()) {
    it->second -= f.shares;
    if (it->second <= 0) {
      id_remaining_.erase(it);
      id_symbol_.erase(id);
    }
  }
  if (on_fill_) on_fill_(id, f);
}

void FillEngine::SetContinuousMatching(bool on) {
  if (continuous_on_ == on) return;
  continuous_on_ = on;
  for (auto& [sym, st] : symbols_) st.model->SetMatchingEnabled(on);
}

void FillEngine::RunOpeningMatch() {
  for (auto& [sym, st] : symbols_) {
    if (st.auction.empty()) continue;
    auto allocs = st.auction.Match();
    std::unordered_map<std::string, long> filled;
    for (const auto& a : allocs) {
      EmitFill(a.id, Fill{a.shares, a.price});
      filled[a.id] += a.shares;
    }
    // Opening remainder carries into the continuous session as a resting order.
    for (const auto& o : st.auction.orders()) {
      long rem = o.shares - filled[o.id];
      if (rem <= 0) continue;
      SimOrder r = o;
      r.filled = o.shares - rem;
      st.model->PlaceResting(r);
    }
    st.auction.Clear();
  }
}

void FillEngine::RunClosingMatch(SymbolState* st) {
  auto allocs = st->auction.Match();
  std::unordered_map<std::string, long> filled;
  for (const auto& a : allocs) {
    EmitFill(a.id, Fill{a.shares, a.price});
    filled[a.id] += a.shares;
  }
  // Closing remainder expires at the close (session over): drop its tracking.
  for (const auto& o : st->auction.orders()) {
    if (o.shares - filled[o.id] > 0) {
      id_remaining_.erase(o.id);
      id_symbol_.erase(o.id);
    }
  }
  st->auction.Clear();
}

void FillEngine::Advance(std::int64_t ts_us) {
  int hhmm = HhmmFromUs(ts_us);
  if (!started_) {
    started_ = true;
    // Pre-open freezes continuous matching; intraday/after start resumes it.
    SetContinuousMatching(hhmm >= kOpenHhmm);
  }

  if (!opening_matched_ && hhmm >= kOpenHhmm) {
    RunOpeningMatch();
    opening_matched_ = true;
    SetContinuousMatching(true);
  } else if (!opening_matched_) {
    SetContinuousMatching(false);
  }

  if (opening_matched_ && !closing_open_ && hhmm >= kCloseWindowStartHhmm) {
    closing_open_ = true;
    SetContinuousMatching(false);
    for (auto& [sym, st] : symbols_) {
      st.closing_ref = st.last_trade;
      st.auction.SetReference(st.closing_ref);
    }
  }

  if (closing_open_) {
    for (auto& [sym, st] : symbols_) {
      if (st.closing_matched || hhmm < st.close_hhmm) continue;
      st.auction.SetReference(st.closing_ref);
      if (!st.closing_delayed && st.auction.DeviatesBeyondBand()) {
        st.closing_delayed = true;
        st.close_hhmm = AddMinutesHhmm(kCloseHhmm, kDelaySeconds / 60);
      } else {
        RunClosingMatch(&st);
        st.closing_matched = true;
      }
    }
  }
}

void FillEngine::OnBook(const std::string& symbol, const TopOfBook& book, std::int64_t ts_us) {
  Advance(ts_us);
  auto it = symbols_.find(symbol);
  if (it != symbols_.end()) it->second.model->OnBook(book, ts_us);
}

void FillEngine::OnTrade(const std::string& symbol, Cents price, long vol, std::int64_t ts_us,
                         bool is_trial) {
  Advance(ts_us);
  auto it = symbols_.find(symbol);
  if (it == symbols_.end()) return;
  if (!is_trial && continuous_on_) it->second.last_trade = price;
  it->second.model->OnTrade(price, vol, ts_us, is_trial);
}

void FillEngine::Submit(const SimOrder& order) {
  if (order.board == Board::kOddLot) {
    if (on_ack_) on_ack_(order.id, false, "odd-lot board not supported");
    return;
  }
  auto it = symbols_.find(order.symbol);
  if (it == symbols_.end()) {
    if (on_ack_) on_ack_(order.id, false, "unknown symbol");
    return;
  }
  Advance(order.place_ts_us);
  id_symbol_[order.id] = order.symbol;
  id_remaining_[order.id] = order.shares;

  SymbolState& st = it->second;
  bool in_opening = !opening_matched_;
  bool in_closing = closing_open_ && !st.closing_matched;
  if (in_opening || in_closing) {
    if (on_ack_) on_ack_(order.id, true, "");  // auction receipt ack
    st.auction.Add(order);
  } else {
    st.model->Submit(order);
  }
}

void FillEngine::Cancel(const std::string& id, std::int64_t ts_us) {
  Advance(ts_us);
  auto it = id_symbol_.find(id);
  if (it == id_symbol_.end()) {
    if (on_cancel_) on_cancel_(id, false);  // unknown / already terminal
    return;
  }
  auto s = symbols_.find(it->second);
  if (s == symbols_.end()) {
    if (on_cancel_) on_cancel_(id, false);
    return;
  }
  // Cancel a resting continuous order, else a still-accumulating auction order.
  if (s->second.model->Cancel(id, ts_us)) return;
  if (s->second.auction.Remove(id)) {
    id_symbol_.erase(id);
    id_remaining_.erase(id);
    if (on_cancel_) on_cancel_(id, true);
    return;
  }
  if (on_cancel_) on_cancel_(id, false);
}

}  // namespace kairos::exec
