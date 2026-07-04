#ifndef KAIROS_EXEC_QUOTE_BOOK_H_
#define KAIROS_EXEC_QUOTE_BOOK_H_

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "tw_market.h"

namespace kairos::exec {

struct Level {
  Cents price = 0;
  long volume = 0;
};

// A distinct TRADE print (schema Trade), delivered separately from the
// DEPTH-bearing Quote so queue models see trades and book updates as separate
// events. `trade_ts_us` is the broker timestamp (CLOCK_REALTIME us).
struct Trade {
  Cents price = 0;
  long volume = 0;
  std::int64_t trade_ts_us = 0;
  bool is_trial = false;  // 試撮
};

struct TopOfBook {
  static constexpr int kMaxLevels = 5;
  std::array<Level, kMaxLevels> bids{};  // [0, n_bids): best -> deep, descending price
  std::array<Level, kMaxLevels> asks{};  // [0, n_asks): best -> deep, ascending price
  int n_bids = 0;
  int n_asks = 0;
  Cents last_trade = 0;
  long last_vol = 0;
  bool is_trial = false;         // 試撮
  std::int64_t quote_ts_us = 0;  // broker quote timestamp (CLOCK_REALTIME us); 0 if absent
  std::chrono::steady_clock::time_point recv_ts{};
  bool valid = false;

  Cents best_bid() const { return n_bids > 0 ? bids[0].price : 0; }
  Cents best_ask() const { return n_asks > 0 ? asks[0].price : 0; }
  bool HasTwoSided() const { return n_bids > 0 && n_asks > 0; }
  // Unrounded midpoint; the caller rounds to the product tick.
  Cents Mid() const { return HasTwoSided() ? (bids[0].price + asks[0].price) / 2 : 0; }
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
