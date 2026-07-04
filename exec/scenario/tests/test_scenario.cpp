// Self-test for the scenario TOML loader + validator. No broker, no network.

#include <unistd.h>

#include <cstdio>
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

  if (g_failures == 0) {
    std::printf("test_scenario: OK\n");
    return 0;
  }
  std::printf("test_scenario: FAILED %d check(s)\n", g_failures);
  return 1;
}
