#ifndef KAIROS_EXEC_SIM_FILL_ENGINE_H_
#define KAIROS_EXEC_SIM_FILL_ENGINE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "fill_model.h"
#include "quote_book.h"
#include "sim_types.h"

namespace kairos::exec {

// Multiplexes per-symbol SymbolFillModels: each symbol's orders and market events
// are fully isolated. Enforces round-lot-only (rejects Board::kOddLot) and
// unknown-symbol submits at the boundary, so the fill logic only ever sees valid
// orders. Not thread-safe by itself: callers (SimOrderBackend) serialize.
class FillEngine {
 public:
  FillEngine(FillMode mode, SimAckFn ack, SimFillFn fill, SimCancelFn cancel);

  // Register a tradable symbol (from the quote subscription). Submits for symbols
  // not registered are rejected.
  void AddSymbol(const std::string& symbol);

  void OnBook(const std::string& symbol, const TopOfBook& book, std::int64_t ts_us);
  void OnTrade(const std::string& symbol, Cents price, long vol, std::int64_t ts_us, bool is_trial);

  // Validates board + symbol, then acks and routes to the symbol's model. Odd-lot
  // or unknown-symbol submits ack ok=false with a reason and never fill.
  void Submit(const SimOrder& order);
  void Cancel(const std::string& id, std::int64_t ts_us);

 private:
  void OnModelFill(const std::string& id, const Fill& f);

  FillMode mode_;
  SimAckFn on_ack_;
  SimFillFn on_fill_;
  SimCancelFn on_cancel_;
  std::unordered_map<std::string, std::unique_ptr<SymbolFillModel>> models_;
  std::unordered_map<std::string, std::string> id_symbol_;  // live order id -> symbol
  std::unordered_map<std::string, long> id_remaining_;      // live order id -> shares left
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SIM_FILL_ENGINE_H_
