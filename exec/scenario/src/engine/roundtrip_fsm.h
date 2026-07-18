#ifndef KAIROS_EXEC_ROUNDTRIP_FSM_H_
#define KAIROS_EXEC_ROUNDTRIP_FSM_H_

// Pure (SDK-free) round-trip state machine. The RoundTripRunner drives sockets,
// timers, and legs; this file only decides the next state + action from an event,
// so every transition stays unit-testable. Step() never throws: an unknown or
// impossible (state, event) pair stays in place and asks for an alert (fail-closed).

#include "scenario.h"  // OnSignalLoss

namespace kairos::exec {

enum class RtState {
  kIdle,   // disarmed (signal lost before entry); re-arms on restore
  kArmed,  // waiting for an in-window enter signal
  kEnter,  // enter leg running
  kHold,   // position held under stop / timeout / forced-exit watchdogs
  kExit,   // exit leg running
  kFlat,   // terminal: no position, done for the day
  kFailed  // terminal: a position may remain (loud fatal)
};

enum class RtEvent {
  kSignalEnter,
  kSignalExit,
  kSignalLost,
  kSignalRestored,
  kEnterLegDone,
  kExitLegDone,
  kStopHit,
  kHoldTimeout,
  kForcedExitTime,
  kQuoteStallHard,
  kLegHalted
};

enum class RtAction {
  kNone,
  kStartEnterLeg,
  kStartExitLeg,
  kDisarm,
  kAlertOnly,
  kTerminalFlat,
  kTerminalFailed
};

// Event payload kept flat so Step() stays a pure function of its input. now_min /
// arm window are minutes since midnight (Taipei); had_fills / position_remaining
// are read from the leg result the runner attaches to a leg-done / halt event.
struct FsmInput {
  RtEvent event;
  int now_min = 0;
  int arm_start_min = 0;
  int arm_end_min = 0;
  OnSignalLoss on_signal_loss = OnSignalLoss::kHoldWithStops;
  bool leg_had_fills = false;
  bool position_remaining = false;
};

struct FsmOutput {
  RtState state;
  RtAction action = RtAction::kNone;
};

FsmOutput Step(RtState state, const FsmInput& in);

const char* RtStateName(RtState s);
const char* RtActionName(RtAction a);

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ROUNDTRIP_FSM_H_
