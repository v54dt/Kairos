#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "json_util.h"
#include "scenario_ctl_proto.h"
#include "test_check.h"

// Cross-language golden: schema/testdata/ctl_corpus.txt pins the scenario-ctl
// JSON-lines protocol. SNAP<TAB> lines are SerializeScenarioSnapshot outputs (all
// states, restart_count/gave_up, hostile last_exit_reason); REQ<TAB> lines are the
// request wire form the tui builders emit (list / start paper|live|test / stop,
// incl. hostile names). The whole file is authored here (SNAP via Serialize, REQ
// via the same JsonEscape the tui json_escape mirrors byte-for-byte); the tui test
// regenerates the REQ lines from its OWN builders and byte-compares (divergence
// catch), and parses the SNAP lines. This C++ test byte-compares the full file and
// parses every REQ line. Trailing '\n' of the wire form is stripped for storage.
//
// Regeneration: KAIROS_REGEN=1 ctest -R test_ctl_corpus.

namespace {

using kairos::exec::JsonEscape;
using kairos::exec::ParseScenarioRequest;
using kairos::exec::ScenarioCmd;
using kairos::exec::ScenarioMode;
using kairos::exec::ScenarioRequest;
using kairos::exec::ScenarioSnapshotRow;
using kairos::exec::SerializeScenarioSnapshot;

// Same hostile string used by the journal corpus: newline, tab, quote, backslash,
// a C0 control (0x1c), CJK.
const std::string kHostile = "reject:\nline2\ttab \"q\" \\slash \x1c sep \xe5\x8f\xb0\xe7\xa9\x8d";
const std::string kHostileName = "ev\"il\n\ttab\\x\x1c\xe5\x8f\xb0";

std::string Strip(const std::string& s) {
  return (!s.empty() && s.back() == '\n') ? s.substr(0, s.size() - 1) : s;
}

ScenarioSnapshotRow Row(const char* name, const char* state, long pid, long cf, long cs, long lft,
                        const std::string& reason, bool live, long rc, bool gu) {
  ScenarioSnapshotRow r;
  r.name = name;
  r.state = state;
  r.pid = pid;
  r.cum_fills = cf;
  r.cum_shares = cs;
  r.last_fill_ts = lft;
  r.last_exit_reason = reason;
  r.live = live;
  r.restart_count = rc;
  r.gave_up = gu;
  return r;
}

std::vector<std::string> SnapLines() {
  std::vector<std::string> out;
  out.push_back(SerializeScenarioSnapshot(true, "", {}));

  std::vector<ScenarioSnapshotRow> all = {
      Row("s-stopped", "stopped", 0, 0, 0, 0, "", false, 0, false),
      Row("s-starting", "starting", 111, 0, 0, 0, "", false, 0, false),
      Row("s-waitopen", "wait-open", 222, 0, 0, 0, "", true, 0, false),
      Row("s-inwindow", "in-window", 333, 5, 5000, 1700000000, "", true, 0, false),
      Row("s-fillrem", "fill-remainder", 444, 2, 2000, 0, "", true, 0, false),
      Row("s-closed", "closed-exited", 0, 0, 0, 0, "window closed / run complete", false, 1, false),
      Row("s-crashed", "crashed", 0, 0, 0, 0, "order backend connect failed", true, 2, false),
      Row("s-stopping", "stopping", 555, 0, 0, 0, "", false, 0, false)};
  out.push_back(SerializeScenarioSnapshot(true, "", all));

  out.push_back(SerializeScenarioSnapshot(
      true, "", {Row("s-give", "crashed", 999, 0, 0, 0, kHostile, true, 3, true)}));

  out.push_back(SerializeScenarioSnapshot(false, "unknown cmd", {}));

  for (std::string& s : out) s = Strip(s);
  return out;
}

std::string StartReq(const std::string& name, const char* mode) {
  return "{\"cmd\":\"start\",\"name\":\"" + JsonEscape(name) + "\",\"mode\":\"" + mode + "\"}";
}
std::string StopReq(const std::string& name) {
  return "{\"cmd\":\"stop\",\"name\":\"" + JsonEscape(name) + "\"}";
}

std::vector<std::string> ReqLines() {
  return {"{\"cmd\":\"list\"}",      StartReq("2330-Buy", "paper"), StartReq("2330-Buy", "live"),
          StartReq("sim-x", "test"), StopReq("2330-Buy"),           StartReq(kHostileName, "paper"),
          StopReq(kHostileName)};
}

const char kHeader[] =
    "# ctl_corpus.txt -- scenario-ctl JSON-lines protocol golden (Track RF1 D).\n"
    "#\n"
    "# SNAP<TAB> = SerializeScenarioSnapshot output (C++ writer; tui parse_snapshot reads).\n"
    "# REQ<TAB>  = tui request builder wire form (tui writer; C++ ParseScenarioRequest reads).\n"
    "# The wire form's trailing newline is stripped for storage. Regeneration:\n"
    "# KAIROS_REGEN=1 ctest -R test_ctl_corpus.\n";

std::string BuildFile() {
  std::string out = kHeader;
  for (const std::string& s : SnapLines()) out += "SNAP\t" + s + "\n";
  for (const std::string& s : ReqLines()) out += "REQ\t" + s + "\n";
  return out;
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
  const char* path = KAIROS_CTL_CORPUS_PATH;
  std::string expected = BuildFile();

  if (const char* regen = std::getenv("KAIROS_REGEN"); regen != nullptr && regen[0] != '\0') {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    CHECK(out.good());
    out << expected;
    std::printf("regenerated %s (%zu bytes)\n", path, expected.size());
    return 0;
  }

  std::string committed = ReadFile(path);
  if (committed != expected) {
    std::printf("FAIL  ctl corpus drift; regenerate with KAIROS_REGEN=1\n");
    return 1;
  }

  // Parse every REQ line and assert the decoded command/name/mode.
  std::vector<std::string> reqs = ReqLines();
  ScenarioRequest r;
  std::string err;
  CHECK(ParseScenarioRequest(reqs[0], &r, &err) && r.cmd == ScenarioCmd::kList);
  CHECK(ParseScenarioRequest(reqs[1], &r, &err) && r.cmd == ScenarioCmd::kStart &&
        r.name == "2330-Buy" && r.mode == ScenarioMode::kPaper);
  CHECK(ParseScenarioRequest(reqs[2], &r, &err) && r.mode == ScenarioMode::kLive);
  CHECK(ParseScenarioRequest(reqs[3], &r, &err) && r.name == "sim-x" &&
        r.mode == ScenarioMode::kTest);
  CHECK(ParseScenarioRequest(reqs[4], &r, &err) && r.cmd == ScenarioCmd::kStop &&
        r.name == "2330-Buy");
  CHECK(ParseScenarioRequest(reqs[5], &r, &err) && r.cmd == ScenarioCmd::kStart &&
        r.name == kHostileName && r.mode == ScenarioMode::kPaper);
  CHECK(ParseScenarioRequest(reqs[6], &r, &err) && r.cmd == ScenarioCmd::kStop &&
        r.name == kHostileName);

  if (g_failures != 0) std::printf("%d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
