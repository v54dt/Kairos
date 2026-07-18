// Self-test for the scenario TOML loader + validator. No broker, no network.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

static bool HasErr(const std::vector<std::string>& errs, const std::string& needle) {
  for (const auto& e : errs)
    if (e.find(needle) != std::string::npos) return true;
  return false;
}

// A minimal enabled [roundtrip] Buy that passes every rule (arm_end 12:00 leaves
// room for enter_window + max_hold before the 13:25 forced exit).
static Scenario ValidRoundTrip() {
  Scenario s;
  s.symbol = "2330";
  s.budget_twd = 300000;
  s.side = Side::kBuy;
  s.roundtrip.enabled = true;
  s.roundtrip.signal = "vwap_reversion";
  s.roundtrip.stop_loss_pct = 1.0;
  s.roundtrip.max_hold_min = 30;
  s.roundtrip.enter_window_min = 10;
  s.roundtrip.arm_start_hhmm = 900;
  s.roundtrip.arm_end_hhmm = 1200;
  return s;
}

// Absent [roundtrip] leaves enabled=false with every field at its default, and an
// enabled block round-trips through the loader and validates.
static void TestRoundTripDefaults() {
  const std::string body = R"(
[scenario]
symbol = "2330"
budget_twd = 300000
)";
  std::string path = WriteTemp(body);
  Scenario s = LoadScenario(path);
  CHECK(!s.roundtrip.enabled);
  CHECK(s.roundtrip.signal.empty());
  CHECK_EQ(s.roundtrip.enter_window_min, 10);
  CHECK(s.roundtrip.on_signal_loss == OnSignalLoss::kHoldWithStops);
  CHECK_EQ(s.roundtrip.arm_start_hhmm, 900);
  CHECK_EQ(s.roundtrip.arm_end_hhmm, 1300);
  CHECK(ValidateScenario(s).empty());  // disabled => no roundtrip errors

  const std::string enabled_body = R"(
[scenario]
symbol = "2330"
budget_twd = 300000
[roundtrip]
enabled = true
signal = "vwap_reversion"
stop_loss_pct = 1.5
max_hold_min = 30
enter_window_min = 5
on_signal_loss = "exit"
arm_start = "09:30"
arm_end = "12:00"
)";
  std::ofstream(path) << enabled_body;
  Scenario e = LoadScenario(path);
  std::remove(path.c_str());
  CHECK(e.roundtrip.enabled);
  CHECK(e.roundtrip.signal == "vwap_reversion");
  CHECK(e.roundtrip.on_signal_loss == OnSignalLoss::kExit);
  CHECK_EQ(e.roundtrip.enter_window_min, 5);
  CHECK_EQ(e.roundtrip.arm_start_hhmm, 930);
  CHECK_EQ(e.roundtrip.arm_end_hhmm, 1200);
  CHECK(ValidateScenario(e).empty());
}

// Every fail-closed reject rule fires; the baseline passes.
static void TestRoundTripValidationMatrix() {
  CHECK(ValidateScenario(ValidRoundTrip()).empty());

  Scenario sell = ValidRoundTrip();
  sell.side = Side::kSell;
  sell.position_shares = 5000;  // satisfy the Sell seatbelt so only the roundtrip rule fires
  CHECK(HasErr(ValidateScenario(sell), "long-only"));

  Scenario no_sig = ValidRoundTrip();
  no_sig.roundtrip.signal.clear();
  CHECK(HasErr(ValidateScenario(no_sig), "roundtrip.signal"));

  Scenario no_stop = ValidRoundTrip();
  no_stop.roundtrip.stop_loss_pct = 0.0;
  CHECK(HasErr(ValidateScenario(no_stop), "stop_loss_pct"));

  Scenario no_hold = ValidRoundTrip();
  no_hold.roundtrip.max_hold_min = 0;
  CHECK(HasErr(ValidateScenario(no_hold), "max_hold_min"));

  Scenario no_win = ValidRoundTrip();
  no_win.roundtrip.enter_window_min = 0;
  CHECK(HasErr(ValidateScenario(no_win), "enter_window_min"));

  Scenario late_arm = ValidRoundTrip();
  late_arm.roundtrip.arm_end_hhmm = 1330;
  CHECK(HasErr(ValidateScenario(late_arm), "arm_end must be <= 13:00"));

  Scenario bad_order = ValidRoundTrip();
  bad_order.roundtrip.arm_start_hhmm = 1200;
  bad_order.roundtrip.arm_end_hhmm = 1200;  // start == end
  CHECK(HasErr(ValidateScenario(bad_order), "arm_start must be before"));
}

// The fit check uses true minutes: arm_end 13:00 (780) + enter + hold == 805 is
// accepted, one minute more is rejected with the arithmetic in the message.
static void TestRoundTripArmArithmetic() {
  Scenario edge = ValidRoundTrip();
  edge.roundtrip.arm_end_hhmm = 1300;
  edge.roundtrip.enter_window_min = 10;
  edge.roundtrip.max_hold_min = 15;  // 780 + 10 + 15 == 805 exactly
  CHECK(ValidateScenario(edge).empty());

  Scenario over = edge;
  over.roundtrip.max_hold_min = 16;  // 806 > 805
  CHECK(HasErr(ValidateScenario(over), "forced exit"));
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
  TestRoundTripDefaults();
  TestRoundTripValidationMatrix();
  TestRoundTripArmArithmetic();

  if (g_failures == 0) {
    std::printf("test_scenario: OK\n");
    return 0;
  }
  std::printf("test_scenario: FAILED %d check(s)\n", g_failures);
  return 1;
}
