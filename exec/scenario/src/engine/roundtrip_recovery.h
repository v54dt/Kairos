#ifndef KAIROS_EXEC_ROUNDTRIP_RECOVERY_H_
#define KAIROS_EXEC_ROUNDTRIP_RECOVERY_H_

// Pure (IO-free) restart-recovery decider for the round-trip runner. Given the net
// position derived from the Buy/Sell fill journals and the facts read from the rt
// journal, it returns what the runner must do BEFORE arming. Keeping it pure makes
// every §7.7 state-point unit-testable with no files, clock, or sockets.
//
// The fill journals are the source of truth for net position (fail-closed: any net
// long means resume HOLD under protection). The rt journal only refines: it marks a
// completed trip (one-trip-per-day survives restart) and supplies the enter-done
// wall-clock anchor for the max-hold countdown.

#include <string>

#include "roundtrip_journal.h"  // RtJournalFacts

namespace kairos::exec {

enum class RecoveryDecision {
  kFresh,           // no position and no prior activity: arm fresh
  kTerminal,        // the day's one trip already ran: do not re-enter
  kResumeHold,      // net long with a recorded enter_done: resume HOLD, anchor from it
  kResumeDegraded,  // net long but no enter_done: resume HOLD, anchor from the first Buy fill
  kRefuse           // net short (impossible for long-only): refuse to start (fail-closed)
};

struct RecoveryInputs {
  long buy_filled = 0;       // total Buy shares in the fill journal
  long buy_notional_c = 0;   // sum of Buy shares*price (cents), for the entry average
  long first_buy_ts_us = 0;  // wall-clock us of the first Buy fill (degraded anchor)
  long sell_filled = 0;      // total Sell shares in the fill journal
  RtJournalFacts rt;
};

struct RecoveryPlan {
  RecoveryDecision decision = RecoveryDecision::kFresh;
  long held_shares = 0;     // net long to resume with
  long entered_shares = 0;  // total Buy shares this day = lifetime shares the exit must sell
  long entry_avg_c = 0;     // average Buy price (cents) for the stop reference
  long enter_wall_us = 0;   // wall-clock anchor the runner turns into remaining max-hold
  bool degraded = false;    // resumed without a recorded enter_done
  std::string message;      // human-readable note for the refuse/resume alert
};

RecoveryPlan DeriveRecovery(const RecoveryInputs& in);

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ROUNDTRIP_RECOVERY_H_
