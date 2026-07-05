#ifndef KAIROS_EXEC_SCENARIO_CTL_PROTO_H_
#define KAIROS_EXEC_SCENARIO_CTL_PROTO_H_

#include <string>
#include <vector>

namespace kairos::exec {

// Longest control line the supervisor will parse; anything longer is rejected
// rather than buffered, so a malformed client cannot grow memory without bound.
constexpr std::size_t kMaxCtlLineLen = 4096;

// The launch mode a start command requests. Fail-closed: there is no default;
// an absent or unrecognized token is rejected and NEVER routed to live.
enum class ScenarioMode { kPaper, kLive, kTest };

// Parse the exact mode token. Only "paper"/"live"/"test" are accepted, and only
// the exact "live" literal maps to kLive; anything else (missing, wrong case,
// trailing space, garbage) returns false.
bool ParseScenarioMode(const std::string& token, ScenarioMode* out);

const char* ScenarioModeName(ScenarioMode mode);

enum class ScenarioCmd { kList, kStart, kStop };

struct ScenarioRequest {
  ScenarioCmd cmd = ScenarioCmd::kList;
  std::string name;                          // start/stop only
  ScenarioMode mode = ScenarioMode::kPaper;  // meaningful only when cmd == kStart
};

// Decode one JSON-lines request. Returns true and fills *out on a valid request;
// returns false and fills *err on any malformed / unknown / fail-closed input
// (unknown cmd, missing name, missing/unknown mode, oversized line). Never throws.
bool ParseScenarioRequest(const std::string& line, ScenarioRequest* out, std::string* err);

// One scenario row in a status snapshot.
struct ScenarioSnapshotRow {
  std::string name;
  std::string state;
  long pid = 0;
  long cum_fills = 0;
  long cum_shares = 0;
  long last_fill_ts = 0;
  std::string last_exit_reason;
  bool live = false;
};

// Serialize the whole response as one JSON line (matches the hand-rolled JSONL
// style of hub_status.cpp): the per-command ok/err plus the snapshot rows.
std::string SerializeScenarioSnapshot(bool ok, const std::string& err,
                                      const std::vector<ScenarioSnapshotRow>& rows);

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SCENARIO_CTL_PROTO_H_
