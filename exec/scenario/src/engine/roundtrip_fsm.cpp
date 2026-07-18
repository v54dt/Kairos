#include "roundtrip_fsm.h"

namespace kairos::exec {

namespace {

FsmOutput StepArmed(const FsmInput& in) {
  switch (in.event) {
    case RtEvent::kSignalEnter:
      if (in.now_min >= in.arm_start_min && in.now_min <= in.arm_end_min)
        return {RtState::kEnter, RtAction::kStartEnterLeg};
      return {RtState::kArmed, RtAction::kAlertOnly};  // out-of-window enter
    case RtEvent::kSignalLost:
      return {RtState::kIdle, RtAction::kDisarm};
    case RtEvent::kSignalRestored:
      return {RtState::kArmed, RtAction::kNone};
    default:
      return {RtState::kArmed, RtAction::kAlertOnly};
  }
}

FsmOutput StepIdle(const FsmInput& in) {
  switch (in.event) {
    case RtEvent::kSignalRestored:
      return {RtState::kArmed, RtAction::kNone};
    case RtEvent::kSignalLost:
      return {RtState::kIdle, RtAction::kNone};  // idempotent
    default:
      return {RtState::kIdle, RtAction::kAlertOnly};
  }
}

FsmOutput StepEnter(const FsmInput& in) {
  switch (in.event) {
    case RtEvent::kEnterLegDone:
      if (in.leg_had_fills) return {RtState::kHold, RtAction::kNone};
      return {RtState::kFlat, RtAction::kTerminalFlat};  // nothing filled to protect
    case RtEvent::kLegHalted:
      if (in.leg_had_fills) return {RtState::kHold, RtAction::kAlertOnly};  // position exists
      return {RtState::kFlat, RtAction::kTerminalFlat};
    default:
      return {RtState::kEnter, RtAction::kAlertOnly};
  }
}

FsmOutput StepHold(const FsmInput& in) {
  switch (in.event) {
    case RtEvent::kSignalExit:
    case RtEvent::kStopHit:
    case RtEvent::kHoldTimeout:
    case RtEvent::kForcedExitTime:
    case RtEvent::kQuoteStallHard:
      return {RtState::kExit, RtAction::kStartExitLeg};
    case RtEvent::kSignalLost:
      if (in.on_signal_loss == OnSignalLoss::kExit)
        return {RtState::kExit, RtAction::kStartExitLeg};
      return {RtState::kHold, RtAction::kAlertOnly};  // hold_with_stops: watchdog stays
    case RtEvent::kSignalRestored:
      return {RtState::kHold, RtAction::kNone};
    default:
      return {RtState::kHold, RtAction::kAlertOnly};
  }
}

FsmOutput StepExit(const FsmInput& in) {
  switch (in.event) {
    case RtEvent::kExitLegDone:
      if (in.position_remaining) return {RtState::kFailed, RtAction::kTerminalFailed};
      return {RtState::kFlat, RtAction::kTerminalFlat};
    case RtEvent::kLegHalted:
      if (in.position_remaining) return {RtState::kFailed, RtAction::kTerminalFailed};
      return {RtState::kFlat, RtAction::kTerminalFlat};
    default:
      return {RtState::kExit, RtAction::kAlertOnly};
  }
}

FsmOutput StepFlat(const FsmInput& in) {
  if (in.event == RtEvent::kSignalEnter)
    return {RtState::kFlat, RtAction::kAlertOnly};  // one round-trip per day: logged, ignored
  return {RtState::kFlat, RtAction::kNone};
}

}  // namespace

FsmOutput Step(RtState state, const FsmInput& in) {
  switch (state) {
    case RtState::kIdle:
      return StepIdle(in);
    case RtState::kArmed:
      return StepArmed(in);
    case RtState::kEnter:
      return StepEnter(in);
    case RtState::kHold:
      return StepHold(in);
    case RtState::kExit:
      return StepExit(in);
    case RtState::kFlat:
      return StepFlat(in);
    case RtState::kFailed:
      return {RtState::kFailed, RtAction::kAlertOnly};  // terminal, keep alerting
  }
  return {state, RtAction::kAlertOnly};
}

const char* RtStateName(RtState s) {
  switch (s) {
    case RtState::kIdle:
      return "idle";
    case RtState::kArmed:
      return "armed";
    case RtState::kEnter:
      return "enter";
    case RtState::kHold:
      return "hold";
    case RtState::kExit:
      return "exit";
    case RtState::kFlat:
      return "flat";
    case RtState::kFailed:
      return "failed";
  }
  return "?";
}

const char* RtActionName(RtAction a) {
  switch (a) {
    case RtAction::kNone:
      return "none";
    case RtAction::kStartEnterLeg:
      return "startEnterLeg";
    case RtAction::kStartExitLeg:
      return "startExitLeg";
    case RtAction::kDisarm:
      return "disarm";
    case RtAction::kAlertOnly:
      return "alertOnly";
    case RtAction::kTerminalFlat:
      return "terminalFlat";
    case RtAction::kTerminalFailed:
      return "terminalFailed";
  }
  return "?";
}

}  // namespace kairos::exec
