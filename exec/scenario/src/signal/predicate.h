#ifndef KAIROS_EXEC_PREDICATE_H_
#define KAIROS_EXEC_PREDICATE_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "quote_book.h"
#include "signal_proto.h"

namespace kairos::exec {

// One firing of a predicate for a single symbol: the side to take plus optional
// flat diagnostic fields carried through to the signal frame.
struct PredicateFire {
  SignalAction action = SignalAction::kEnter;
  std::map<std::string, std::string> fields;
};

// A firing tagged with the symbol it fired for (Poll can emit several at once).
struct PredicateHit {
  std::string symbol;
  PredicateFire fire;
};

// SDK-free predicate seam. A predicate watches a fixed symbol set and keeps its
// own per-symbol state; each hook returns a fire only on a state transition, so
// the engine can drive it on synthetic sequences in a unit test. `ts_us` is a
// monotonic microsecond stamp supplied by the caller (injected in tests).
class Predicate {
 public:
  virtual ~Predicate() = default;

  const std::string& Name() const { return name_; }
  const std::vector<std::string>& Symbols() const { return symbols_; }
  bool WatchesSymbol(const std::string& symbol) const {
    for (const std::string& s : symbols_)
      if (s == symbol) return true;
    return false;
  }
  // Whether the predicate's symbols belong in the quote UDS subscription; a
  // poll-only predicate (manual) does not consume quotes.
  virtual bool NeedsQuotes() const { return true; }

  virtual std::optional<PredicateFire> OnQuote(const std::string& /*symbol*/,
                                               const TopOfBook& /*tob*/, std::int64_t /*ts_us*/) {
    return std::nullopt;
  }
  virtual std::optional<PredicateFire> OnTrade(const std::string& /*symbol*/,
                                               const Trade& /*trade*/, std::int64_t /*ts_us*/) {
    return std::nullopt;
  }
  // Time/spool-driven predicates (e.g. manual) emit here; quote-driven ones don't.
  virtual std::vector<PredicateHit> Poll(std::int64_t /*ts_us*/) { return {}; }

 protected:
  Predicate(std::string name, std::vector<std::string> symbols)
      : name_(std::move(name)), symbols_(std::move(symbols)) {}

  std::string name_;
  std::vector<std::string> symbols_;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_PREDICATE_H_
