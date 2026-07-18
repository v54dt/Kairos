// Exhaustive transition-matrix cover for the pure round-trip FSM: every (state,
// event) cell, both signal-loss policy branches, one-trip-per-day, and the
// unknown/impossible-event fail-closed no-op (stay + alert, never throw).

#include <cstdio>

#include "roundtrip_fsm.h"
#include "test_check.h"

using namespace kairos::exec;

namespace {

// arm window [09:00, 13:00] in minutes; now inside it unless a test overrides.
FsmInput In(RtEvent e) {
  FsmInput in;
  in.event = e;
  in.now_min = 10 * 60;  // 10:00, inside the arm window
  in.arm_start_min = 9 * 60;
  in.arm_end_min = 13 * 60;
  return in;
}

void Expect(RtState from, const FsmInput& in, RtState to, RtAction act) {
  FsmOutput out = Step(from, in);
  CHECK(out.state == to);
  CHECK(out.action == act);
}

void TestArmed() {
  Expect(RtState::kArmed, In(RtEvent::kSignalEnter), RtState::kEnter, RtAction::kStartEnterLeg);
  FsmInput out_of_window = In(RtEvent::kSignalEnter);
  out_of_window.now_min = 8 * 60;  // before arm_start
  Expect(RtState::kArmed, out_of_window, RtState::kArmed, RtAction::kAlertOnly);
  out_of_window.now_min = 14 * 60;  // after arm_end
  Expect(RtState::kArmed, out_of_window, RtState::kArmed, RtAction::kAlertOnly);
  Expect(RtState::kArmed, In(RtEvent::kSignalLost), RtState::kIdle, RtAction::kDisarm);
  Expect(RtState::kArmed, In(RtEvent::kSignalRestored), RtState::kArmed, RtAction::kNone);
  Expect(RtState::kArmed, In(RtEvent::kSignalExit), RtState::kArmed, RtAction::kAlertOnly);
  Expect(RtState::kArmed, In(RtEvent::kStopHit), RtState::kArmed, RtAction::kAlertOnly);
  Expect(RtState::kArmed, In(RtEvent::kEnterLegDone), RtState::kArmed, RtAction::kAlertOnly);
}

void TestIdle() {
  Expect(RtState::kIdle, In(RtEvent::kSignalRestored), RtState::kArmed, RtAction::kNone);
  Expect(RtState::kIdle, In(RtEvent::kSignalLost), RtState::kIdle, RtAction::kNone);
  Expect(RtState::kIdle, In(RtEvent::kSignalEnter), RtState::kIdle, RtAction::kAlertOnly);
  Expect(RtState::kIdle, In(RtEvent::kStopHit), RtState::kIdle, RtAction::kAlertOnly);
}

void TestEnter() {
  FsmInput filled = In(RtEvent::kEnterLegDone);
  filled.leg_had_fills = true;
  Expect(RtState::kEnter, filled, RtState::kHold, RtAction::kNone);
  FsmInput empty = In(RtEvent::kEnterLegDone);
  empty.leg_had_fills = false;
  Expect(RtState::kEnter, empty, RtState::kFlat, RtAction::kTerminalFlat);

  FsmInput halt_filled = In(RtEvent::kLegHalted);
  halt_filled.leg_had_fills = true;
  Expect(RtState::kEnter, halt_filled, RtState::kHold, RtAction::kAlertOnly);
  FsmInput halt_empty = In(RtEvent::kLegHalted);
  halt_empty.leg_had_fills = false;
  Expect(RtState::kEnter, halt_empty, RtState::kFlat, RtAction::kTerminalFlat);

  Expect(RtState::kEnter, In(RtEvent::kSignalExit), RtState::kEnter, RtAction::kAlertOnly);
  Expect(RtState::kEnter, In(RtEvent::kStopHit), RtState::kEnter, RtAction::kAlertOnly);
}

void TestHold() {
  Expect(RtState::kHold, In(RtEvent::kSignalExit), RtState::kExit, RtAction::kStartExitLeg);
  Expect(RtState::kHold, In(RtEvent::kStopHit), RtState::kExit, RtAction::kStartExitLeg);
  Expect(RtState::kHold, In(RtEvent::kHoldTimeout), RtState::kExit, RtAction::kStartExitLeg);
  Expect(RtState::kHold, In(RtEvent::kForcedExitTime), RtState::kExit, RtAction::kStartExitLeg);
  Expect(RtState::kHold, In(RtEvent::kQuoteStallHard), RtState::kExit, RtAction::kStartExitLeg);

  FsmInput lost_exit = In(RtEvent::kSignalLost);
  lost_exit.on_signal_loss = OnSignalLoss::kExit;
  Expect(RtState::kHold, lost_exit, RtState::kExit, RtAction::kStartExitLeg);
  FsmInput lost_hold = In(RtEvent::kSignalLost);
  lost_hold.on_signal_loss = OnSignalLoss::kHoldWithStops;
  Expect(RtState::kHold, lost_hold, RtState::kHold, RtAction::kAlertOnly);

  Expect(RtState::kHold, In(RtEvent::kSignalRestored), RtState::kHold, RtAction::kNone);
  Expect(RtState::kHold, In(RtEvent::kSignalEnter), RtState::kHold, RtAction::kAlertOnly);
  Expect(RtState::kHold, In(RtEvent::kEnterLegDone), RtState::kHold, RtAction::kAlertOnly);
}

void TestExit() {
  FsmInput done_flat = In(RtEvent::kExitLegDone);
  done_flat.position_remaining = false;
  Expect(RtState::kExit, done_flat, RtState::kFlat, RtAction::kTerminalFlat);
  FsmInput done_left = In(RtEvent::kExitLegDone);
  done_left.position_remaining = true;
  Expect(RtState::kExit, done_left, RtState::kFailed, RtAction::kTerminalFailed);

  FsmInput halt_left = In(RtEvent::kLegHalted);
  halt_left.position_remaining = true;
  Expect(RtState::kExit, halt_left, RtState::kFailed, RtAction::kTerminalFailed);
  FsmInput halt_flat = In(RtEvent::kLegHalted);
  halt_flat.position_remaining = false;
  Expect(RtState::kExit, halt_flat, RtState::kFlat, RtAction::kTerminalFlat);

  Expect(RtState::kExit, In(RtEvent::kSignalExit), RtState::kExit, RtAction::kAlertOnly);
  Expect(RtState::kExit, In(RtEvent::kStopHit), RtState::kExit, RtAction::kAlertOnly);
}

void TestFlat() {
  Expect(RtState::kFlat, In(RtEvent::kSignalEnter), RtState::kFlat, RtAction::kAlertOnly);
  Expect(RtState::kFlat, In(RtEvent::kSignalExit), RtState::kFlat, RtAction::kNone);
  Expect(RtState::kFlat, In(RtEvent::kSignalLost), RtState::kFlat, RtAction::kNone);
  Expect(RtState::kFlat, In(RtEvent::kStopHit), RtState::kFlat, RtAction::kNone);
}

void TestFailed() {
  Expect(RtState::kFailed, In(RtEvent::kSignalEnter), RtState::kFailed, RtAction::kAlertOnly);
  Expect(RtState::kFailed, In(RtEvent::kExitLegDone), RtState::kFailed, RtAction::kAlertOnly);
  Expect(RtState::kFailed, In(RtEvent::kSignalRestored), RtState::kFailed, RtAction::kAlertOnly);
}

}  // namespace

int main() {
  TestArmed();
  TestIdle();
  TestEnter();
  TestHold();
  TestExit();
  TestFlat();
  TestFailed();
  if (g_failures == 0) {
    std::printf("test_roundtrip_fsm: OK\n");
    return 0;
  }
  std::printf("test_roundtrip_fsm: FAILED %d check(s)\n", g_failures);
  return 1;
}
