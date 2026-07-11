// Self-test for the scenario TOML loader + validator. No broker, no network.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "order_journal.h"  // ResolveJournalDir
#include "scenario.h"
#include "test_check.h"

using namespace kairos::exec;

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

// The parser leaves [journal].dir empty when unset (the live-only default lives in
// the engine, so paper never inherits the live journal path); an explicit dir wins.
static void TestJournalDefault() {
  const std::string body = R"(
[scenario]
symbol = "0050"
budget_twd = 1000
)";
  ::setenv("HOME", "/tmp/kairos-home-xyz", 1);
  std::string path = WriteTemp(body);
  Scenario s = LoadScenario(path);
  CHECK(s.journal_dir.empty());

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

// The shared journal-dir resolver both mains use. The engine form (no legacy env)
// and the hub form (legacy KAIROS_HUB_JOURNAL_DIR fallback) must agree on the
// shared KAIROS_JOURNAL_DIR, with the per-side toml always winning, so the trader
// and the hub never split the restart journal.
static void TestResolveJournalDir() {
  ::setenv("HOME", "/tmp/kairos-home-xyz", 1);
  ::unsetenv("KAIROS_JOURNAL_DIR");
  ::unsetenv("KAIROS_HUB_JOURNAL_DIR");

  // No config, no env: both sides fall back to the same $HOME default.
  CHECK(ResolveJournalDir("", nullptr) == "/tmp/kairos-home-xyz/Kairos/data/journal");
  CHECK(ResolveJournalDir("", "KAIROS_HUB_JOURNAL_DIR") ==
        "/tmp/kairos-home-xyz/Kairos/data/journal");

  // The shared var is honored by BOTH forms, and they resolve the SAME directory.
  ::setenv("KAIROS_JOURNAL_DIR", "/srv/shared/journal", 1);
  CHECK(ResolveJournalDir("", nullptr) == "/srv/shared/journal");
  bool used_legacy = true;
  CHECK(ResolveJournalDir("", "KAIROS_HUB_JOURNAL_DIR", &used_legacy) == "/srv/shared/journal");
  CHECK(!used_legacy);  // the shared var, not the legacy fallback, supplied it
  CHECK(ResolveJournalDir("", nullptr) == ResolveJournalDir("", "KAIROS_HUB_JOURNAL_DIR"));

  // Per-side toml wins over every env var, on both forms.
  CHECK(ResolveJournalDir("/etc/kairos/journal", nullptr) == "/etc/kairos/journal");
  CHECK(ResolveJournalDir("/etc/kairos/journal", "KAIROS_HUB_JOURNAL_DIR") ==
        "/etc/kairos/journal");

  // Only the deprecated var set: the hub form falls back to it and flags the use;
  // the engine form (no legacy) drops to the $HOME default instead.
  ::unsetenv("KAIROS_JOURNAL_DIR");
  ::setenv("KAIROS_HUB_JOURNAL_DIR", "/legacy/hub/journal", 1);
  used_legacy = false;
  CHECK(ResolveJournalDir("", "KAIROS_HUB_JOURNAL_DIR", &used_legacy) == "/legacy/hub/journal");
  CHECK(used_legacy);
  CHECK(ResolveJournalDir("", nullptr) == "/tmp/kairos-home-xyz/Kairos/data/journal");

  // The shared var takes precedence over the deprecated one when both are set.
  ::setenv("KAIROS_JOURNAL_DIR", "/srv/shared/journal", 1);
  used_legacy = true;
  CHECK(ResolveJournalDir("", "KAIROS_HUB_JOURNAL_DIR", &used_legacy) == "/srv/shared/journal");
  CHECK(!used_legacy);

  ::unsetenv("KAIROS_JOURNAL_DIR");
  ::unsetenv("KAIROS_HUB_JOURNAL_DIR");
}

static Scenario ValidBuy() {
  Scenario s;
  s.symbol = "2330";
  s.budget_twd = 300000;
  return s;
}

// budget_twd XOR budget_shares, the Sell position_shares seatbelt, and the
// round-lot 1000-share multiples for the new share-denominated fields.
static void TestSellAndBudgetValidation() {
  CHECK(ValidateScenario(ValidBuy()).empty());

  // exactly one of budget_twd / budget_shares must be > 0
  Scenario both = ValidBuy();
  both.budget_shares = 1000;  // budget_twd also > 0
  CHECK(!ValidateScenario(both).empty());
  Scenario neither = ValidBuy();
  neither.budget_twd = 0;
  CHECK(!ValidateScenario(neither).empty());
  Scenario shares_buy = ValidBuy();
  shares_buy.budget_twd = 0;
  shares_buy.budget_shares = 2000;  // buy + budget_shares is valid
  CHECK(ValidateScenario(shares_buy).empty());

  // Sell requires position_shares > 0 (fail-closed seatbelt)
  Scenario sell_no_pos = ValidBuy();
  sell_no_pos.side = Side::kSell;
  CHECK(!ValidateScenario(sell_no_pos).empty());
  Scenario sell_ok = ValidBuy();
  sell_ok.side = Side::kSell;
  sell_ok.position_shares = 5000;
  CHECK(ValidateScenario(sell_ok).empty());

  // Sell budget_shares must not exceed the held position
  Scenario over = ValidBuy();
  over.side = Side::kSell;
  over.budget_twd = 0;
  over.budget_shares = 6000;
  over.position_shares = 5000;
  CHECK(!ValidateScenario(over).empty());
  Scenario under = over;
  under.budget_shares = 5000;  // == position is allowed
  CHECK(ValidateScenario(under).empty());

  // RoundLot requires 1000-share multiples for the new fields
  Scenario rl_budget = ValidBuy();
  rl_budget.board = Board::kRoundLot;
  rl_budget.budget_twd = 0;
  rl_budget.budget_shares = 1500;
  CHECK(!ValidateScenario(rl_budget).empty());
  Scenario rl_pos = ValidBuy();
  rl_pos.board = Board::kRoundLot;
  rl_pos.side = Side::kSell;
  rl_pos.position_shares = 1500;
  CHECK(!ValidateScenario(rl_pos).empty());
}

// --budget writes budget_twd, but on a share-denominated scenario it must fail
// fast with an explicit message rather than tripping the generic XOR check.
static void TestBudgetOverride() {
  // budget_twd scenario: the override updates the budget and succeeds.
  Scenario twd = ValidBuy();
  std::string err;
  CHECK(ApplyBudgetOverride(500000, &twd, &err));
  CHECK_EQ(twd.budget_twd, 500000);
  CHECK(err.empty());

  // A non-positive override is a no-op.
  Scenario noop = ValidBuy();
  CHECK(ApplyBudgetOverride(0, &noop, &err));
  CHECK_EQ(noop.budget_twd, 300000);

  // budget_shares scenario: the override is refused with the explicit message.
  Scenario shares = ValidBuy();
  shares.budget_twd = 0;
  shares.budget_shares = 2000;
  err.clear();
  CHECK(!ApplyBudgetOverride(500000, &shares, &err));
  CHECK(err.find("budget_shares") != std::string::npos);
  CHECK(err.find("edit the toml") != std::string::npos);
  CHECK_EQ(shares.budget_twd, 0);  // unchanged: no partial write
  CHECK_EQ(shares.budget_shares, 2000);
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
  TestResolveJournalDir();
  TestSellAndBudgetValidation();
  TestBudgetOverride();

  if (g_failures == 0) {
    std::printf("test_scenario: OK\n");
    return 0;
  }
  std::printf("test_scenario: FAILED %d check(s)\n", g_failures);
  return 1;
}
