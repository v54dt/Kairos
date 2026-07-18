#include "roundtrip_recovery.h"

namespace kairos::exec {

RecoveryPlan DeriveRecovery(const RecoveryInputs& in) {
  RecoveryPlan p;
  long net = in.buy_filled - in.sell_filled;

  if (net < 0) {
    p.decision = RecoveryDecision::kRefuse;
    p.message = "sell fills (" + std::to_string(in.sell_filled) + ") exceed buy fills (" +
                std::to_string(in.buy_filled) + "): inconsistent books, refusing to start";
    return p;
  }

  if (net == 0) {
    if (in.rt.has_trigger || in.rt.terminal) {
      p.decision = RecoveryDecision::kTerminal;
      p.message = "the day's round trip already ran; not re-entering";
    } else {
      p.decision = RecoveryDecision::kFresh;
    }
    return p;
  }

  // net > 0: we still hold a position. Fail-closed — always resume HOLD so the stop
  // watchdog protects it, regardless of what the rt journal says.
  p.held_shares = net;
  p.entry_avg_c = in.buy_filled > 0 ? in.buy_notional_c / in.buy_filled : 0;
  if (in.rt.has_enter_done) {
    p.decision = RecoveryDecision::kResumeHold;
    p.enter_wall_us = in.rt.enter_ts_us;
    p.message = "resuming HOLD of " + std::to_string(net) + " sh from the rt journal";
  } else {
    p.decision = RecoveryDecision::kResumeDegraded;
    p.degraded = true;
    p.enter_wall_us = in.first_buy_ts_us;
    p.message = "degraded recovery: buy fills without an enter_done line; resuming HOLD of " +
                std::to_string(net) + " sh, max-hold anchored to the first Buy fill";
  }
  return p;
}

}  // namespace kairos::exec
