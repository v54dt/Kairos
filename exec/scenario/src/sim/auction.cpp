#include "auction.h"

#include <algorithm>
#include <cstdlib>

#include "session_schedule.h"

namespace kairos::exec {

void AuctionEngine::Add(const SimOrder& order) { orders_.push_back(order); }

bool AuctionEngine::Remove(const std::string& id) {
  for (auto it = orders_.begin(); it != orders_.end(); ++it) {
    if (it->id == id) {
      orders_.erase(it);
      return true;
    }
  }
  return false;
}

AuctionEngine::Cross AuctionEngine::ComputeCross() const {
  Cross best;
  long best_imbalance = 0;
  for (const auto& cand : orders_) {
    Cents p = cand.price;
    if (p <= 0) continue;
    long demand = 0;
    long supply = 0;
    for (const auto& o : orders_) {
      long q = o.remaining();
      if (q <= 0) continue;
      if (o.side == Side::kBuy) {
        if (o.price >= p) demand += q;
      } else {
        if (o.price <= p) supply += q;
      }
    }
    long exec = std::min(demand, supply);
    if (exec <= 0) continue;
    long imbalance = std::labs(demand - supply);
    bool better;
    if (!best.crossed) {
      better = true;
    } else if (exec != best.volume) {
      better = exec > best.volume;
    } else if (imbalance != best_imbalance) {
      better = imbalance < best_imbalance;
    } else if (reference_ > 0 &&
               std::llabs(p - reference_) != std::llabs(best.price - reference_)) {
      better = std::llabs(p - reference_) < std::llabs(best.price - reference_);
    } else {
      better = p > best.price;
    }
    if (better) {
      best.crossed = true;
      best.price = p;
      best.volume = exec;
      best_imbalance = imbalance;
    }
  }
  return best;
}

bool AuctionEngine::DeviatesBeyondBand() const {
  if (reference_ <= 0) return false;
  Cross c = ComputeCross();
  if (!c.crossed) return false;
  return std::llabs(c.price - reference_) * kStabilizationDen > reference_ * kStabilizationNum;
}

std::vector<AuctionEngine::Alloc> AuctionEngine::Match() const {
  Cross c = ComputeCross();
  std::vector<Alloc> out;
  if (!c.crossed) return out;
  Cents p = c.price;

  // Eligible orders per side, tagged with arrival index for a stable tie-break.
  std::vector<int> buys, sells;
  for (int i = 0; i < static_cast<int>(orders_.size()); ++i) {
    const auto& o = orders_[i];
    if (o.remaining() <= 0) continue;
    if (o.side == Side::kBuy) {
      if (o.price >= p) buys.push_back(i);
    } else {
      if (o.price <= p) sells.push_back(i);
    }
  }

  // Buys: price desc, then time (place_ts asc), then arrival order.
  std::sort(buys.begin(), buys.end(), [this](int a, int b) {
    const auto& x = orders_[a];
    const auto& y = orders_[b];
    if (x.price != y.price) return x.price > y.price;
    if (x.place_ts_us != y.place_ts_us) return x.place_ts_us < y.place_ts_us;
    return a < b;
  });
  // Sells: price asc, then time, then arrival order.
  std::sort(sells.begin(), sells.end(), [this](int a, int b) {
    const auto& x = orders_[a];
    const auto& y = orders_[b];
    if (x.price != y.price) return x.price < y.price;
    if (x.place_ts_us != y.place_ts_us) return x.place_ts_us < y.place_ts_us;
    return a < b;
  });

  auto allocate = [&](const std::vector<int>& side) {
    long left = c.volume;
    for (int idx : side) {
      if (left <= 0) break;
      long take = std::min(orders_[idx].remaining(), left);
      if (take <= 0) continue;
      out.push_back({orders_[idx].id, take, p});
      left -= take;
    }
  };
  allocate(buys);
  allocate(sells);
  return out;
}

}  // namespace kairos::exec
