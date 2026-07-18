#include "roundtrip_legs.h"

#include <algorithm>
#include <format>
#include <string>

namespace kairos::exec {

namespace {

std::string MinutesToHhmmStr(int minutes) {
  return std::format("{:02d}:{:02d}", minutes / 60, minutes % 60);
}

void SetWindow(Scenario& s, int start_min, int end_min) {
  s.window_start = MinutesToHhmmStr(start_min);
  s.window_end = MinutesToHhmmStr(end_min);
  s.window_start_hhmm = MinutesToHhmm(start_min);
  s.window_end_hhmm = MinutesToHhmm(end_min);
}

}  // namespace

Scenario DeriveEnterLeg(const Scenario& parent, int trigger_min) {
  Scenario s = parent;
  s.roundtrip.enabled = false;
  s.side = Side::kBuy;
  int start = std::min(trigger_min, kForcedExitMin);
  int end = std::min(trigger_min + parent.roundtrip.enter_window_min, kForcedExitMin);
  if (end < start) end = start;
  SetWindow(s, start, end);
  return s;
}

Scenario DeriveExitLeg(const Scenario& parent, long entered_shares, ExitReason reason,
                       int now_min) {
  Scenario s = parent;
  s.roundtrip.enabled = false;
  s.side = Side::kSell;
  s.budget_twd = 0;
  s.budget_shares = entered_shares;
  s.position_shares = entered_shares;
  s.fees.daytrade = true;
  if (reason != ExitReason::kReverseSignal) s.price_policy = PricePolicy::kCross;
  SetWindow(s, std::min(now_min, kForcedExitMin), kForcedExitMin);
  return s;
}

}  // namespace kairos::exec
