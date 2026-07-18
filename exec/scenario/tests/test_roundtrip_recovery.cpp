// Pure DeriveRecovery matrix: every §7.7 restart state-point, no IO. Fill journals
// are the source of truth for net position; the rt journal only refines terminal
// status and the enter-done anchor.

#include <cstdio>

#include "roundtrip_recovery.h"
#include "test_check.h"

using namespace kairos::exec;

namespace {

RtJournalFacts Rt(bool armed, bool trigger, bool enter_done, long sh, long avg, long ts,
                  bool terminal) {
  RtJournalFacts f;
  f.has_armed = armed;
  f.has_trigger = trigger;
  f.has_enter_done = enter_done;
  f.enter_shares = sh;
  f.enter_avg_cents = avg;
  f.enter_ts_us = ts;
  f.terminal = terminal;
  return f;
}

}  // namespace

int main() {
  // net == 0, no rt activity => fresh IDLE->ARMED.
  {
    RecoveryInputs in;
    in.rt = Rt(false, false, false, 0, 0, 0, false);
    auto p = DeriveRecovery(in);
    CHECK(p.decision == RecoveryDecision::kFresh);
  }

  // net == 0 but rt shows a trigger => the day's trip already ran; do not re-enter.
  {
    RecoveryInputs in;
    in.rt = Rt(true, true, false, 0, 0, 0, false);
    auto p = DeriveRecovery(in);
    CHECK(p.decision == RecoveryDecision::kTerminal);
  }

  // net == 0 with a terminal rt (bought then fully sold, flat line written).
  {
    RecoveryInputs in;
    in.buy_filled = 300;
    in.sell_filled = 300;
    in.rt = Rt(true, true, true, 300, 58000, 100, true);
    auto p = DeriveRecovery(in);
    CHECK(p.decision == RecoveryDecision::kTerminal);
  }

  // net > 0 with a recorded enter_done => resume HOLD, avg from Buy notional/shares,
  // anchor from the enter_done wall ts.
  {
    RecoveryInputs in;
    in.buy_filled = 300;
    in.buy_notional_c = 300 * 58000;
    in.first_buy_ts_us = 1000;
    in.sell_filled = 0;
    in.rt = Rt(true, true, true, 300, 58000, 7777, false);
    auto p = DeriveRecovery(in);
    CHECK(p.decision == RecoveryDecision::kResumeHold);
    CHECK(p.held_shares == 300);
    CHECK(p.entry_avg_c == 58000);
    CHECK(p.enter_wall_us == 7777);  // enter_done anchor, not first-buy
    CHECK(!p.degraded);
  }

  // net > 0 after a partial exit: held = buy - sell, avg still the Buy average.
  {
    RecoveryInputs in;
    in.buy_filled = 300;
    in.buy_notional_c = 300 * 58000;
    in.sell_filled = 100;
    in.rt = Rt(true, true, true, 300, 58000, 5000, false);
    auto p = DeriveRecovery(in);
    CHECK(p.decision == RecoveryDecision::kResumeHold);
    CHECK(p.held_shares == 200);
    CHECK(p.entry_avg_c == 58000);
  }

  // net > 0 but NO enter_done (crash before the rt result line landed): degraded
  // resume, anchor = first Buy fill ts (conservative).
  {
    RecoveryInputs in;
    in.buy_filled = 300;
    in.buy_notional_c = 300 * 57000;
    in.first_buy_ts_us = 4242;
    in.rt = Rt(true, true, false, 0, 0, 0, false);
    auto p = DeriveRecovery(in);
    CHECK(p.decision == RecoveryDecision::kResumeDegraded);
    CHECK(p.held_shares == 300);
    CHECK(p.entry_avg_c == 57000);
    CHECK(p.enter_wall_us == 4242);
    CHECK(p.degraded);
  }

  // net > 0 with NO rt journal at all (fills exist, rt file missing): still degraded
  // resume, never a fresh re-arm.
  {
    RecoveryInputs in;
    in.buy_filled = 100;
    in.buy_notional_c = 100 * 60000;
    in.first_buy_ts_us = 11;
    in.rt = Rt(false, false, false, 0, 0, 0, false);
    auto p = DeriveRecovery(in);
    CHECK(p.decision == RecoveryDecision::kResumeDegraded);
    CHECK(p.held_shares == 100);
  }

  // net < 0 (impossible for long-only): refuse to start, loud fail-closed.
  {
    RecoveryInputs in;
    in.buy_filled = 100;
    in.sell_filled = 300;
    auto p = DeriveRecovery(in);
    CHECK(p.decision == RecoveryDecision::kRefuse);
    CHECK(!p.message.empty());
  }

  if (g_failures == 0) {
    std::printf("test_roundtrip_recovery: OK\n");
    return 0;
  }
  std::printf("test_roundtrip_recovery: FAILED %d check(s)\n", g_failures);
  return 1;
}
