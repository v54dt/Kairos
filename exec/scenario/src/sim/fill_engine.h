#ifndef KAIROS_EXEC_SIM_FILL_ENGINE_H_
#define KAIROS_EXEC_SIM_FILL_ENGINE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "auction.h"
#include "fill_model.h"
#include "quote_book.h"
#include "session_schedule.h"
#include "sim_types.h"

namespace kairos::exec {

// Multiplexes per-symbol continuous fills (SymbolFillModel) and the opening /
// closing call auctions (AuctionEngine) over an event-time-driven session clock.
// Each symbol is fully isolated. Round-lot-only and unknown-symbol submits are
// rejected at the boundary. Session phase (pre-open / continuous / closing
// auction / closed) is derived only from event timestamps — no wall clock — and
// event timestamps are assumed monotonic non-decreasing (a replayed tape).
// Not thread-safe by itself: callers (SimOrderBackend) serialize.
class FillEngine {
 public:
  FillEngine(FillMode mode, SimAckFn ack, SimFillFn fill, SimCancelFn cancel);

  // Register a tradable symbol (from the quote subscription). Submits for symbols
  // not registered are rejected.
  void AddSymbol(const std::string& symbol);

  void OnBook(const std::string& symbol, const TopOfBook& book, std::int64_t ts_us);
  void OnTrade(const std::string& symbol, Cents price, long vol, std::int64_t ts_us, bool is_trial);

  // Validates board + symbol, acks, then routes: pre-open -> opening auction,
  // closing window -> closing auction, otherwise the continuous model. Odd-lot or
  // unknown-symbol submits ack ok=false with a reason and never fill.
  void Submit(const SimOrder& order);
  void Cancel(const std::string& id, std::int64_t ts_us);

 private:
  struct SymbolState {
    std::unique_ptr<SymbolFillModel> model;
    AuctionEngine auction;
    Cents last_trade = 0;   // last continuous non-trial trade (closing reference source)
    Cents closing_ref = 0;  // snapshot at the closing window open
    bool closing_matched = false;
    bool closing_delayed = false;
    int close_hhmm = kCloseHhmm;  // effective close, extended once on 延緩收盤
  };

  void Advance(std::int64_t ts_us);     // drive session-phase transitions
  void SetContinuousMatching(bool on);  // freeze/resume all continuous models
  void RunOpeningMatch();
  void RunClosingMatch(SymbolState* st);
  void EmitFill(const std::string& id, const Fill& f);  // fill + id bookkeeping

  FillMode mode_;
  SimAckFn on_ack_;
  SimFillFn on_fill_;
  SimCancelFn on_cancel_;
  std::unordered_map<std::string, SymbolState> symbols_;
  std::unordered_map<std::string, std::string> id_symbol_;  // live order id -> symbol
  std::unordered_map<std::string, long> id_remaining_;      // live order id -> shares left

  bool started_ = false;          // first Advance seen
  bool opening_matched_ = false;  // opening auction resolved (or skipped intraday)
  bool closing_open_ = false;     // closing accumulation window entered
  bool continuous_on_ = true;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SIM_FILL_ENGINE_H_
