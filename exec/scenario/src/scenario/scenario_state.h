#ifndef KAIROS_EXEC_SCENARIO_STATE_H_
#define KAIROS_EXEC_SCENARIO_STATE_H_

#include <string>
#include <vector>

#include "scenario_ctl_proto.h"

namespace kairos::exec {

// Per-scenario lifecycle. The daemon owns the child, so the terminal states come
// straight from waitpid; the intermediate phases are inferred from the lines the
// trader already prints. The daemon never recomputes the trading window.
enum class ScenarioState {
  kStopped,
  kStarting,
  kWaitOpen,
  kInWindow,
  kFillRemainder,
  kClosedExited,
  kCrashed,
  kStopping,
};

const char* StateName(ScenarioState state);

// Fill/share counters accumulated from the trader's stdout.
struct ScenarioCounters {
  long cum_fills = 0;
  long cum_shares = 0;
  long last_fill_ts = 0;
};

// Feed one captured child output line into the state machine. On a fill line it
// increments *counters (cum_fills) and sets cum_shares to the cumulative value
// the trader reports, stamping last_fill_ts with `now_ts`. Returns the advanced
// state. Phase is inferred purely from the trader's output (banner -> wait-open,
// fill -> in-window, progress 100% -> fill-remainder); the window is never
// recomputed here.
ScenarioState ApplyStdoutLine(ScenarioState cur, const std::string& line,
                              ScenarioCounters* counters, long now_ts);

// If `line` is one of the trader's known fatal messages, return a short human
// reason for a crash; otherwise "". The monitor keeps the last non-empty result.
std::string ExtractFailureReason(const std::string& line);

struct ExitOutcome {
  ScenarioState state;
  std::string reason;
};

// Terminal classification from the owned child's wait status. requested_stop wins
// (an operator SIGINT is never mislabeled as a natural close); else a signal or
// non-zero code is a crash; else a clean exit that reached the trader's end line
// is closed-exited. `last_fail_reason` is the last ExtractFailureReason hit and
// disambiguates a code-0 early bail (e.g. connect failed) as a crash.
ExitOutcome ClassifyExit(bool requested_stop, int wait_status, bool saw_end_line,
                         const std::string& last_fail_reason);

// The exact argv for a start, reproducing the TUI's build_spawn_argv plus the
// daemon-only test variant. paper=[bin,toml]; live=[bin,toml,--live,--yes];
// test=[bin,toml,--ignore-window,--ignore-blacklist]. Only live carries --live.
std::vector<std::string> BuildTraderArgv(const std::string& bin, const std::string& toml,
                                         ScenarioMode mode);

// Resolve the trader binary: explicit flag, else $KAIROS_SCENARIO_TRADER, else
// <scenario_dir>/build/kairos_scenario_trader (mirrors the TUI resolver).
std::string ResolveTraderBin(const std::string& flag, const char* env,
                             const std::string& scenario_dir);

// A launchable scenario discovered in the scenario dir.
struct ScenarioInfo {
  std::string name;    // toml stem
  std::string symbol;  // [scenario] symbol (non-empty == launchable)
  std::string path;    // absolute toml path to spawn
};

// Enumerate launchable scenario tomls in `dir`, reproducing the TUI rules: only
// *.toml, skip *.example.*, skip a symbol-less config (e.g. hub.toml), name =
// stem. Sorted by path. An unreadable/unparseable toml is skipped, never fatal.
std::vector<ScenarioInfo> EnumerateScenarios(const std::string& dir);

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SCENARIO_STATE_H_
