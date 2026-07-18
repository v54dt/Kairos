// ValidateScenario: roundtrip.stop_loss_pct must lie strictly in (0, 100). At or
// above 100 the stop can never fire, so it is rejected with a message naming the
// bound. Only this specific error is asserted; unrelated defaults do not matter.

#include <cstdio>
#include <string>
#include <vector>

#include "scenario.h"
#include "test_check.h"

using namespace kairos::exec;

namespace {

Scenario RtScenario(double stop_loss_pct) {
  Scenario s;
  s.symbol = "2330";
  s.budget_twd = 100000;
  s.side = Side::kBuy;
  s.roundtrip.enabled = true;
  s.roundtrip.signal = "vwap";
  s.roundtrip.stop_loss_pct = stop_loss_pct;
  s.roundtrip.max_hold_min = 30;
  s.roundtrip.enter_window_min = 10;
  s.roundtrip.arm_start_hhmm = 900;
  s.roundtrip.arm_end_hhmm = 1200;
  return s;
}

bool HasBoundError(const std::vector<std::string>& errs) {
  for (const auto& e : errs)
    if (e.find("stop_loss_pct must be > 0 and < 100") != std::string::npos) return true;
  return false;
}

}  // namespace

int main() {
  // Rejected: at/below 0 and at/above 100.
  CHECK(HasBoundError(ValidateScenario(RtScenario(0.0))));
  CHECK(HasBoundError(ValidateScenario(RtScenario(-1.0))));
  CHECK(HasBoundError(ValidateScenario(RtScenario(100.0))));
  CHECK(HasBoundError(ValidateScenario(RtScenario(150.0))));

  // Accepted band: strictly inside (0, 100).
  CHECK(!HasBoundError(ValidateScenario(RtScenario(0.5))));
  CHECK(!HasBoundError(ValidateScenario(RtScenario(2.0))));
  CHECK(!HasBoundError(ValidateScenario(RtScenario(99.9))));

  if (g_failures == 0) {
    std::printf("test_scenario_stop_loss_bound: OK\n");
    return 0;
  }
  std::printf("test_scenario_stop_loss_bound: FAILED %d check(s)\n", g_failures);
  return 1;
}
