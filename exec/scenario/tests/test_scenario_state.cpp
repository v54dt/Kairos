// Unit test: the per-scenario state machine + spawn/enumeration helpers. Pins the
// exact engine.cpp fill/banner/progress strings so a future trader format change
// breaks here, covers every state edge and the clean-exit vs crash reason split,
// and checks the argv per mode + the enumeration rules.

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "scenario_state.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

// The exact lines engine.cpp prints (kept verbatim so the parser stays pinned).
static const char* kBanner = "kairos-exec: Buy 2330 NT$ 300000, cross, PAPER";
static const char* kBannerLive = "kairos-exec: Sell 2454 NT$ 500000, join, *** LIVE ***";
static const char* kFill =
    "kairos-exec: fill k1234-1 1000 @ 925.00  (cum 3000 sh / NT$ 2775000, fee 3953)";
static const char* kProgress100 = "kairos-exec: progress 100%";
static const char* kProgress50 = "kairos-exec: progress 50%";
static const char* kEnd = "kairos-exec: end - filled 3000 sh / NT$ 2775000 of 300000, fee NT$ 3953";

static int ExitStatus(int code) {
  // Encode a WIFEXITED status with the given exit code.
  return (code & 0xff) << 8;
}

int main() {
  // starting -> wait-open on the banner (paper and live banners both).
  {
    ScenarioCounters c;
    CHECK(ApplyStdoutLine(ScenarioState::kStarting, kBanner, &c, 100) == ScenarioState::kWaitOpen);
    CHECK(ApplyStdoutLine(ScenarioState::kStarting, kBannerLive, &c, 100) ==
          ScenarioState::kWaitOpen);
  }
  // wait-open -> in-window on a fill; cum_fills/cum_shares/last_fill_ts parsed.
  {
    ScenarioCounters c;
    ScenarioState s = ApplyStdoutLine(ScenarioState::kWaitOpen, kFill, &c, 4242);
    CHECK(s == ScenarioState::kInWindow);
    CHECK(c.cum_fills == 1);
    CHECK(c.cum_shares == 3000);  // cumulative from "(cum 3000 sh"
    CHECK(c.last_fill_ts == 4242);
    // A second fill increments the count and updates the cumulative.
    const char* fill2 =
        "kairos-exec: fill k1234-2 500 @ 925.50  (cum 3500 sh / NT$ 3238000, fee 4611)";
    s = ApplyStdoutLine(s, fill2, &c, 4300);
    CHECK(c.cum_fills == 2);
    CHECK(c.cum_shares == 3500);
    CHECK(c.last_fill_ts == 4300);
  }
  // in-window -> fill-remainder on progress 100%; 50% does not advance.
  {
    ScenarioCounters c;
    CHECK(ApplyStdoutLine(ScenarioState::kInWindow, kProgress50, &c, 0) ==
          ScenarioState::kInWindow);
    CHECK(ApplyStdoutLine(ScenarioState::kInWindow, kProgress100, &c, 0) ==
          ScenarioState::kFillRemainder);
    // A fill after fill-remainder keeps fill-remainder (still counts).
    ScenarioState s = ApplyStdoutLine(ScenarioState::kFillRemainder, kFill, &c, 5000);
    CHECK(s == ScenarioState::kFillRemainder);
    CHECK(c.cum_fills == 1);
  }
  // An unrelated line does not change state or counters.
  {
    ScenarioCounters c;
    ScenarioState s = ApplyStdoutLine(ScenarioState::kInWindow, "some other log line", &c, 7);
    CHECK(s == ScenarioState::kInWindow);
    CHECK(c.cum_fills == 0);
  }

  // ClassifyExit: operator stop wins regardless of a clean exit + end line.
  {
    ExitOutcome o = ClassifyExit(/*requested_stop=*/true, ExitStatus(0), /*saw_end_line=*/true, "");
    CHECK(o.state == ScenarioState::kStopped);
    CHECK(o.reason == "stopped by operator");
  }
  // Clean exit 0 with the end line -> closed-exited (the off-hours window-close).
  {
    ExitOutcome o = ClassifyExit(false, ExitStatus(0), true, "");
    CHECK(o.state == ScenarioState::kClosedExited);
    CHECK(o.reason == "window closed / run complete");
  }
  // Non-zero exit -> crashed; reason from the captured fatal line when present.
  {
    ExitOutcome o = ClassifyExit(false, ExitStatus(1), false, "blacklist gate refused");
    CHECK(o.state == ScenarioState::kCrashed);
    CHECK(o.reason == "blacklist gate refused");
    ExitOutcome o2 = ClassifyExit(false, ExitStatus(3), false, "");
    CHECK(o2.state == ScenarioState::kCrashed);
    CHECK(o2.reason == "exited with code 3");
  }
  // Killed by a signal -> crashed.
  {
    ExitOutcome o = ClassifyExit(false, SIGKILL, false, "");  // WIFSIGNALED, sig in low bits
    CHECK(o.state == ScenarioState::kCrashed);
    CHECK(o.reason == "killed by signal 9");
  }
  // Exit 0 but bailed before the end line with a fatal line -> crashed.
  {
    ExitOutcome o = ClassifyExit(false, ExitStatus(0), false, "order backend connect failed");
    CHECK(o.state == ScenarioState::kCrashed);
    CHECK(o.reason == "order backend connect failed");
  }

  // ExtractFailureReason pins the trader's fatal lines.
  CHECK(ExtractFailureReason("kairos-exec: order backend connect failed") ==
        "order backend connect failed");
  CHECK(ExtractFailureReason("kairos-exec: order backend disconnected; stopping") ==
        "order backend disconnected");
  CHECK(ExtractFailureReason("REFUSING TO TRADE: 2330 is blacklisted") == "blacklist gate refused");
  CHECK(ExtractFailureReason(kEnd).empty());
  CHECK(ExtractFailureReason(kFill).empty());

  // BuildTraderArgv per mode.
  {
    auto paper = BuildTraderArgv("/b/trader", "/s/2330.toml", ScenarioMode::kPaper);
    CHECK((paper == std::vector<std::string>{"/b/trader", "/s/2330.toml"}));

    auto live = BuildTraderArgv("/b/trader", "/s/2330.toml", ScenarioMode::kLive);
    CHECK((live == std::vector<std::string>{"/b/trader", "/s/2330.toml", "--live", "--yes"}));

    auto test = BuildTraderArgv("/b/trader", "/s/2330.toml", ScenarioMode::kTest);
    CHECK((test == std::vector<std::string>{"/b/trader", "/s/2330.toml", "--ignore-window",
                                            "--ignore-blacklist"}));
    // Only live carries --live.
    CHECK(std::find(paper.begin(), paper.end(), "--live") == paper.end());
    CHECK(std::find(test.begin(), test.end(), "--live") == test.end());
  }

  // ResolveTraderBin precedence: flag > env > derived default.
  {
    CHECK(ResolveTraderBin("/flag/bin", "/env/bin", "/scn") == "/flag/bin");
    CHECK(ResolveTraderBin("", "/env/bin", "/scn") == "/env/bin");
    CHECK(ResolveTraderBin("", nullptr, "/scn") == "/scn/build/kairos_scenario_trader");
    CHECK(ResolveTraderBin("", "", "/scn") == "/scn/build/kairos_scenario_trader");
  }

  // EnumerateScenarios: launchable stems only; skip example/hub/non-toml.
  {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("kairos-scn-enum-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    auto write = [&](const std::string& fname, const std::string& body) {
      std::ofstream f(dir / fname);
      f << body;
    };
    write("2330.toml", "[scenario]\nname=\"a\"\nsymbol=\"2330\"\n");
    write("2454.toml", "[scenario]\nname=\"b\"\nsymbol=\"2454\"\n");
    write("scenario.example.toml", "[scenario]\nsymbol=\"9999\"\n");  // template: skipped
    write("hub.toml", "[user]\nuser_id=\"x\"\n");                     // no symbol: skipped
    write("notes.txt", "not a toml");                                 // non-toml: skipped
    write("bad.toml", "this is = not valid = toml [[[");              // unparseable: skipped

    auto found = EnumerateScenarios(dir.string());
    CHECK(found.size() == 2);
    if (found.size() == 2) {
      CHECK(found[0].name == "2330");
      CHECK(found[0].symbol == "2330");
      CHECK(found[1].name == "2454");
    }
    std::filesystem::remove_all(dir);
  }

  if (g_failures == 0) {
    std::printf("test_scenario_state: OK\n");
    return 0;
  }
  std::printf("test_scenario_state: FAILED %d check(s)\n", g_failures);
  return 1;
}
