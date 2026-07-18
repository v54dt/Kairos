#ifndef KAIROS_EXEC_PREDICATE_DEPTH_EVAP_H_
#define KAIROS_EXEC_PREDICATE_DEPTH_EVAP_H_

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "predicate.h"

namespace kairos::exec {

// Which side's five-level depth the predicate watches for evaporation.
enum class DepthSide { kBid, kAsk };

struct DepthEvapParams {
  DepthSide side = DepthSide::kBid;
  double window_s = 10.0;        // EWMA time-constant of the depth baseline
  double ratio_enter = 0.4;      // fire enter when depth / baseline drops below this
  double ratio_exit = 0.7;       // fire exit only once it recovers above this
  double warmup_s = 5.0;         // suppress firing until the baseline has settled
  std::int64_t cooldown_us = 0;  // min gap between successive enter fires
};

// Fires when the watched side's total five-level volume collapses relative to a
// rolling EWMA baseline. Dual enter/exit thresholds plus a cooldown and a frozen
// baseline while fired keep a flickering book from machine-gunning signals. Pure:
// all timing comes from the `ts_us` argument.
class DepthEvapPredicate : public Predicate {
 public:
  DepthEvapPredicate(std::string name, std::vector<std::string> symbols, DepthEvapParams params);

  std::optional<PredicateFire> OnQuote(const std::string& symbol, const TopOfBook& tob,
                                       std::int64_t ts_us) override;

 private:
  struct SymbolState {
    bool have_baseline = false;
    bool fired = false;
    double baseline = 0.0;
    std::int64_t first_ts = 0;
    std::int64_t last_ts = 0;
    std::int64_t last_enter_ts = 0;
    bool have_enter = false;
  };

  DepthEvapParams params_;
  std::unordered_map<std::string, SymbolState> state_;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_PREDICATE_DEPTH_EVAP_H_
