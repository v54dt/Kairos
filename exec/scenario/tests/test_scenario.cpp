// Self-test for the scenario TOML loader + validator. No broker, no network.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "scenario.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                                   \
  do {                                                                                   \
    auto _a = (a);                                                                       \
    auto _b = (b);                                                                       \
    if (!(_a == _b)) {                                                                   \
      std::printf("FAIL  %s:%d  %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                  (long long)_a, (long long)_b);                                         \
      ++g_failures;                                                                      \
    }                                                                                    \
  } while (0)

static std::string WriteTemp(const std::string& body) {
  std::string path = "/tmp/kairos-scenario-test-" + std::to_string(getpid()) + ".toml";
  std::ofstream(path) << body;
  return path;
}

// base = "..." inheritance: shared defaults, overridden per scenario, with nested
// tables deep-merged (not replaced).
static void TestBaseMerge() {
  std::string dir = "/tmp/kairos-scenario-base-" + std::to_string(getpid());
  std::filesystem::create_directories(dir);
  std::ofstream(dir + "/base.toml") << R"(
[scenario]
name = "base"
symbol = "0050"
market = "TSE"
board = "OddLot"
side = "Buy"
product = "etf"
budget_twd = 300000
[pricing]
policy = "join"
peg_level = 1
[window]
start_time = "09:10"
end_time = "13:25"
)";
  std::ofstream(dir + "/child.toml") << R"(
base = "base.toml"
[scenario]
symbol = "2330"
side = "Sell"
budget_twd = 999000
[pricing]
peg_level = 3
)";

  Scenario s = LoadScenario(dir + "/child.toml");
  // overridden by the child
  CHECK(s.symbol == "2330");
  CHECK(s.side == Side::kSell);
  CHECK_EQ(s.budget_twd, 999000);
  CHECK_EQ(s.peg_level, 3);  // nested [pricing] merged, not replaced
  // inherited from the base
  CHECK(s.board == Board::kOddLot);
  CHECK(s.price_policy == PricePolicy::kJoin);  // survives the nested merge
  CHECK_EQ(s.window_start_hhmm, 910);
  CHECK_EQ(s.window_end_hhmm, 1325);

  std::filesystem::remove_all(dir);
}

// [journal].dir defaults to $HOME/Kairos/data/journal (HOME expanded at load);
// an explicit dir wins.
static void TestJournalDefault() {
  const std::string body = R"(
[scenario]
symbol = "0050"
budget_twd = 1000
)";
  ::setenv("HOME", "/tmp/kairos-home-xyz", 1);
  std::string path = WriteTemp(body);
  Scenario s = LoadScenario(path);
  CHECK(s.journal_dir == "/tmp/kairos-home-xyz/Kairos/data/journal");

  const std::string explicit_body = R"(
[scenario]
symbol = "0050"
budget_twd = 1000
[journal]
dir = "/var/lib/kairos/journal"
)";
  std::ofstream(path) << explicit_body;
  Scenario s2 = LoadScenario(path);
  CHECK(s2.journal_dir == "/var/lib/kairos/journal");  // explicit value wins over the default
  std::remove(path.c_str());
}

int main() {
  const std::string body = R"(
[scenario]
name = "test-etf"
symbol = "0050"
market = "TSE"
board = "OddLot"
side = "Buy"
product = "etf"
budget_twd = 300000
pacing = "twap"

[pricing]
policy = "join"
use_trial_quotes = true

[window]
start_time = "09:10"
end_time = "13:25"

[risk]
quote_max_age_ms = 70000
)";

  std::string path = WriteTemp(body);
  Scenario s = LoadScenario(path);
  std::remove(path.c_str());

  CHECK(s.symbol == "0050");
  CHECK(s.product == Product::kEtf);
  CHECK(s.board == Board::kOddLot);
  CHECK(s.side == Side::kBuy);
  CHECK(s.price_policy == PricePolicy::kJoin);
  CHECK(s.use_trial_quotes);
  CHECK_EQ(s.budget_twd, 300000);
  CHECK_EQ(s.window_start_hhmm, 910);
  CHECK_EQ(s.window_end_hhmm, 1325);
  CHECK_EQ(s.quote_max_age_ms, 70000);
  CHECK(ValidateScenario(s).empty());

  // validation catches bad configs
  Scenario bad = s;
  bad.budget_twd = 0;
  CHECK(!ValidateScenario(bad).empty());
  bad = s;
  bad.symbol = "";
  CHECK(!ValidateScenario(bad).empty());
  bad = s;
  bad.window_start_hhmm = 1400;  // after end
  CHECK(!ValidateScenario(bad).empty());

  // summary renders without crashing and mentions the symbol
  CHECK(SummarizeScenario(s).find("0050") != std::string::npos);

  TestBaseMerge();
  TestJournalDefault();

  if (g_failures == 0) {
    std::printf("test_scenario: OK\n");
    return 0;
  }
  std::printf("test_scenario: FAILED %d check(s)\n", g_failures);
  return 1;
}
