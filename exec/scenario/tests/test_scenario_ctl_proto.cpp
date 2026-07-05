// Unit test: the scenario control JSON-lines protocol. Encode/decode round-trips
// and every fail-closed rejection (unknown cmd, missing name, missing/unknown
// mode, malformed/partial/oversized input). Only the exact "live" token routes
// to kLive.

#include <cstdio>
#include <string>
#include <vector>

#include "scenario_ctl_proto.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

static bool Accepts(const std::string& line, ScenarioRequest* out) {
  std::string err;
  return ParseScenarioRequest(line, out, &err);
}

static bool Rejects(const std::string& line) {
  ScenarioRequest req;
  std::string err;
  return !ParseScenarioRequest(line, &req, &err) && !err.empty();
}

int main() {
  // list
  {
    ScenarioRequest req;
    CHECK(Accepts(R"({"cmd":"list"})", &req));
    CHECK(req.cmd == ScenarioCmd::kList);
  }
  // start with each valid mode
  {
    ScenarioRequest req;
    CHECK(Accepts(R"({"cmd":"start","name":"2330","mode":"paper"})", &req));
    CHECK(req.cmd == ScenarioCmd::kStart);
    CHECK(req.name == "2330");
    CHECK(req.mode == ScenarioMode::kPaper);

    CHECK(Accepts(R"({"cmd":"start","name":"2330","mode":"live"})", &req));
    CHECK(req.mode == ScenarioMode::kLive);

    CHECK(Accepts(R"({"cmd":"start","name":"2330","mode":"test"})", &req));
    CHECK(req.mode == ScenarioMode::kTest);
  }
  // key order independence
  {
    ScenarioRequest req;
    CHECK(Accepts(R"({"mode":"test","name":"2454","cmd":"start"})", &req));
    CHECK(req.cmd == ScenarioCmd::kStart);
    CHECK(req.name == "2454");
    CHECK(req.mode == ScenarioMode::kTest);
  }
  // stop
  {
    ScenarioRequest req;
    CHECK(Accepts(R"({"cmd":"stop","name":"2330"})", &req));
    CHECK(req.cmd == ScenarioCmd::kStop);
    CHECK(req.name == "2330");
  }

  // FAIL-CLOSED: only the exact "live" literal maps to kLive.
  {
    ScenarioMode m;
    CHECK(ParseScenarioMode("live", &m) && m == ScenarioMode::kLive);
    CHECK(!ParseScenarioMode("LIVE", &m));
    CHECK(!ParseScenarioMode("Live", &m));
    CHECK(!ParseScenarioMode("l1ve", &m));
    CHECK(!ParseScenarioMode("live ", &m));
    CHECK(!ParseScenarioMode(" live", &m));
    CHECK(!ParseScenarioMode("", &m));
    CHECK(!ParseScenarioMode("paper ", &m));
  }

  // A garbled mode is rejected and never routed to a start (so never to --live).
  for (const char* mode : {"LIVE", "l1ve", "paper ", "real", "1", ""}) {
    std::string line = R"({"cmd":"start","name":"2330","mode":")" + std::string(mode) + "\"}";
    CHECK(Rejects(line));
  }

  // unknown cmd
  CHECK(Rejects(R"({"cmd":"frobnicate"})"));
  CHECK(Rejects(R"({"cmd":"restart","name":"2330"})"));
  // missing cmd
  CHECK(Rejects(R"({"name":"2330"})"));
  // start without name
  CHECK(Rejects(R"({"cmd":"start","mode":"paper"})"));
  CHECK(Rejects(R"({"cmd":"start","name":"","mode":"paper"})"));
  // start with missing mode
  CHECK(Rejects(R"({"cmd":"start","name":"2330"})"));
  // stop without name
  CHECK(Rejects(R"({"cmd":"stop"})"));

  // malformed / partial / junk
  CHECK(Rejects(""));
  CHECK(Rejects("{"));
  CHECK(Rejects(R"({"cmd":"list")"));        // no closing brace
  CHECK(Rejects(R"({"cmd":"list",})"));      // trailing comma
  CHECK(Rejects(R"({"cmd":42})"));           // non-string value
  CHECK(Rejects(R"({"cmd":"list"} junk)"));  // trailing garbage
  CHECK(Rejects("not json at all"));
  CHECK(Rejects(std::string("{\"cmd\":\"li\0st\"}", 15)));  // embedded NUL bytes

  // oversized line is rejected without buffering.
  {
    std::string big = R"({"cmd":"start","name":")" + std::string(kMaxCtlLineLen, 'x') + "\"}";
    CHECK(Rejects(big));
  }

  // whitespace-tolerant valid line
  {
    ScenarioRequest req;
    CHECK(Accepts("  { \"cmd\" : \"list\" }  ", &req));
    CHECK(req.cmd == ScenarioCmd::kList);
  }

  // Snapshot serialization emits every field.
  {
    std::vector<ScenarioSnapshotRow> rows;
    rows.push_back({"2330", "in-window", 4242, 3, 6000, 1720000000, "", true});
    rows.push_back({"2317", "stopped", 0, 0, 0, 0, "window closed", false});
    std::string s = SerializeScenarioSnapshot(true, "", rows);
    CHECK(s.find("\"ok\":true") != std::string::npos);
    CHECK(s.find("\"name\":\"2330\"") != std::string::npos);
    CHECK(s.find("\"state\":\"in-window\"") != std::string::npos);
    CHECK(s.find("\"pid\":4242") != std::string::npos);
    CHECK(s.find("\"cum_fills\":3") != std::string::npos);
    CHECK(s.find("\"cum_shares\":6000") != std::string::npos);
    CHECK(s.find("\"last_fill_ts\":1720000000") != std::string::npos);
    CHECK(s.find("\"live\":true") != std::string::npos);
    CHECK(s.find("\"last_exit_reason\":\"window closed\"") != std::string::npos);
    CHECK(s.find("\"live\":false") != std::string::npos);
    CHECK(!s.empty() && s.back() == '\n');
  }
  // An error response carries ok=false + the reason.
  {
    std::string s = SerializeScenarioSnapshot(false, "unknown mode", {});
    CHECK(s.find("\"ok\":false") != std::string::npos);
    CHECK(s.find("\"err\":\"unknown mode\"") != std::string::npos);
    CHECK(s.find("\"scenarios\":[]") != std::string::npos);
  }

  if (g_failures == 0) {
    std::printf("test_scenario_ctl_proto: OK\n");
    return 0;
  }
  std::printf("test_scenario_ctl_proto: FAILED %d check(s)\n", g_failures);
  return 1;
}
