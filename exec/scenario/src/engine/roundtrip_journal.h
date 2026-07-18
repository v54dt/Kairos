#ifndef KAIROS_EXEC_ROUNDTRIP_JOURNAL_H_
#define KAIROS_EXEC_ROUNDTRIP_JOURNAL_H_

// Append-only round-trip lifecycle journal: <symbol>-rt-<day>.jsonl, one JSON line
// per event, in the SAME directory as the per-leg fill journals. It records the
// trip's decisions and results so a restart can tell a completed trip from an open
// position and re-arm the stop watchdog. It is NOT a fill record — net position is
// always derived from the Buy/Sell fill journals; this file only supplies the
// one-trip-per-day status and the enter-done wall-clock anchor for max-hold.
//
// Crash-consistency: DECISION lines (trigger, exit_trigger) are written BEFORE the
// action they commit to, so a crash in the gap over-states commitment (fail-closed:
// never re-enter, always protect). RESULT lines (enter_done, flat, failed) are
// written AFTER, with the fills as the durable backstop. Money-adjacent lines fsync;
// the pre-arm "armed" note does not. Handle-less writes reuse the O_APPEND single-
// write discipline so a trader and the hub can share the directory without tearing.

#include <string>

namespace kairos::exec {

// Handle-less writer. Each method opens, appends one line, fsyncs (except Arm), and
// closes. `dir`/`name` name the file (<name> == "<symbol>-rt-<day>"). Return value
// mirrors AppendJsonlLine (false on write/open error); callers log, never crash.
class RoundTripJournal {
 public:
  static bool Arm(const std::string& dir, const std::string& name);
  static bool Trigger(const std::string& dir, const std::string& name, const std::string& signal,
                      long seq, long ts_us);
  static bool EnterDone(const std::string& dir, const std::string& name, long shares,
                        long avg_px_cents, long ts_us);
  static bool ExitTrigger(const std::string& dir, const std::string& name,
                          const std::string& reason, long ts_us);
  static bool Flat(const std::string& dir, const std::string& name, long ts_us);
  static bool Failed(const std::string& dir, const std::string& name, const std::string& reason,
                     long shares_remaining, long ts_us);
};

// Facts a restart reads back from the rt journal. Malformed/unknown lines are
// skipped (fail-tolerant read); the decisions the runner makes from these facts are
// fail-closed. `terminal` is set once a flat OR failed line is seen.
struct RtJournalFacts {
  bool has_armed = false;
  bool has_trigger = false;
  bool has_enter_done = false;
  long enter_shares = 0;
  long enter_avg_cents = 0;
  long enter_ts_us = 0;
  bool terminal = false;
};

RtJournalFacts ReadRoundTripJournal(const std::string& path);

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ROUNDTRIP_JOURNAL_H_
