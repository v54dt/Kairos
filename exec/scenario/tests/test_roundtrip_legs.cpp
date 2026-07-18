// Pure-function coverage for the round-trip leg derivation (no engine, no SDK).

#include <cstdio>

#include "roundtrip_legs.h"
#include "scenario.h"
#include "test_check.h"

using namespace kairos::exec;

namespace {

Scenario Parent() {
  Scenario s;
  s.symbol = "2330";
  s.side = Side::kBuy;
  s.board = Board::kOddLot;
  s.budget_twd = 300000;
  s.price_policy = PricePolicy::kJoin;
  s.journal_dir = "/var/lib/kairos/journal";
  s.roundtrip.enabled = true;
  s.roundtrip.enter_window_min = 10;
  return s;
}

// Enter leg: Buy, parent budget/pricing/journal preserved, window sits at the
// trigger and both HH:MM strings and hhmm ints stay coherent.
void TestEnterLegWindowAndBudget() {
  Scenario e = DeriveEnterLeg(Parent(), 600);  // 10:00
  CHECK(e.side == Side::kBuy);
  CHECK_EQ(e.budget_twd, 300000);
  CHECK(e.price_policy == PricePolicy::kJoin);
  CHECK_EQ(e.window_start_hhmm, 1000);
  CHECK_EQ(e.window_end_hhmm, 1010);
  CHECK(e.window_start == "10:00");
  CHECK(e.window_end == "10:10");
  CHECK(e.journal_dir == "/var/lib/kairos/journal");
}

// The enter window clamps to the 13:25 forced exit at both ends.
void TestEnterLegClampsToForcedExit() {
  Scenario p = Parent();
  p.roundtrip.enter_window_min = 30;
  Scenario e = DeriveEnterLeg(p, 800);  // 13:20 + 30 -> end clamps to 13:25
  CHECK_EQ(e.window_start_hhmm, 1320);
  CHECK_EQ(e.window_end_hhmm, 1325);

  Scenario late = DeriveEnterLeg(p, 810);  // 13:30 -> both ends clamp to 13:25
  CHECK_EQ(late.window_start_hhmm, 1325);
  CHECK_EQ(late.window_end_hhmm, 1325);
}

// Exit leg: Sell of exactly entered_shares, budget/position propagated, a same-day
// close (daytrade), windowed [now, 13:25].
void TestExitLegSharesAndDaytrade() {
  Scenario x = DeriveExitLeg(Parent(), 1234, ExitReason::kStopLoss, 700);  // 11:40
  CHECK(x.side == Side::kSell);
  CHECK_EQ(x.budget_shares, 1234);
  CHECK_EQ(x.position_shares, 1234);
  CHECK_EQ(x.budget_twd, 0);
  CHECK(x.fees.daytrade);
  CHECK_EQ(x.window_start_hhmm, 1140);
  CHECK_EQ(x.window_end_hhmm, 1325);
}

// Pricing by reason: reverse-signal keeps the parent policy, everything else crosses.
void TestExitLegPricingByReason() {
  Scenario p = Parent();  // parent policy = join
  CHECK(DeriveExitLeg(p, 100, ExitReason::kReverseSignal, 700).price_policy == PricePolicy::kJoin);
  CHECK(DeriveExitLeg(p, 100, ExitReason::kStopLoss, 700).price_policy == PricePolicy::kCross);
  CHECK(DeriveExitLeg(p, 100, ExitReason::kHoldTimeout, 700).price_policy == PricePolicy::kCross);
  CHECK(DeriveExitLeg(p, 100, ExitReason::kForcedTime, 700).price_policy == PricePolicy::kCross);
}

// A now_min past the forced exit clamps the exit window to 13:25.
void TestExitLegWindowClampsNow() {
  Scenario x = DeriveExitLeg(Parent(), 100, ExitReason::kForcedTime, 810);
  CHECK_EQ(x.window_start_hhmm, 1325);
  CHECK_EQ(x.window_end_hhmm, 1325);
}

}  // namespace

int main() {
  TestEnterLegWindowAndBudget();
  TestEnterLegClampsToForcedExit();
  TestExitLegSharesAndDaytrade();
  TestExitLegPricingByReason();
  TestExitLegWindowClampsNow();
  if (g_failures == 0) {
    std::printf("test_roundtrip_legs: OK\n");
    return 0;
  }
  std::printf("test_roundtrip_legs: FAILED %d check(s)\n", g_failures);
  return 1;
}
