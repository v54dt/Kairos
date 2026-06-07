#ifndef KAIROS_EXEC_QUOTE_BOOK_H_
#define KAIROS_EXEC_QUOTE_BOOK_H_

#include <chrono>
#include <mutex>

#include "tw_market.h"

namespace kairos::exec {

struct TopOfBook {
  Cents best_bid = 0;
  long best_bid_vol = 0;
  Cents best_ask = 0;
  long best_ask_vol = 0;
  Cents last_trade = 0;
  long last_vol = 0;
  bool is_trial = false;  // 試撮
  std::chrono::system_clock::time_point quote_ts{};
  std::chrono::steady_clock::time_point recv_ts{};
  bool valid = false;

  bool HasTwoSided() const { return best_bid > 0 && best_ask > 0; }
  // Unrounded midpoint; the caller rounds to the product tick.
  Cents Mid() const { return HasTwoSided() ? (best_bid + best_ask) / 2 : 0; }
};

// Thread-safe latest snapshot: the quote feed writes on its own thread, the
// scheduler reads on the trading thread.
class QuoteBook {
 public:
  void Update(const TopOfBook& tob) {
    std::lock_guard<std::mutex> lock(mu_);
    tob_ = tob;
  }

  TopOfBook Snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return tob_;
  }

  long AgeMs() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!tob_.valid) return -1;
    auto d = std::chrono::steady_clock::now() - tob_.recv_ts;
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
  }

 private:
  mutable std::mutex mu_;
  TopOfBook tob_;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_QUOTE_BOOK_H_
