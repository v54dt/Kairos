#ifndef KAIROS_EXEC_SCENARIO_H_
#define KAIROS_EXEC_SCENARIO_H_

// Declarative scenario (劇本) config consumed by the execution engine. The TOML
// loader / validator / summary live in scenario.cpp.

#include <string>
#include <vector>

#include "dashboard_metrics.h"
#include "enum_names.h"  // Board/Side/Market/Pacing/PricePolicy + *Name
#include "notify_config.h"
#include "tw_fees.h"
#include "tw_market.h"

namespace kairos::exec {

struct UserCreds {
  std::string user_id;
  std::string password;
  std::string account;
  std::string pfx_filepath;
  std::string pfx_password;
};

// What the round-trip does when the entry signal fades before the position is
// closed: keep the position under stop/timeout, or exit immediately.
enum class OnSignalLoss { kHoldWithStops, kExit };

// [roundtrip] same-day long round-trip: enter on a signal, exit on reverse
// signal / stop-loss / max hold / the 13:25 forced-exit wall time. All fields
// are validated only when enabled, so an absent table changes nothing.
struct RoundTripConfig {
  bool enabled = false;
  std::string signal;          // entry predicate name (required when enabled)
  double stop_loss_pct = 0.0;  // required > 0
  int max_hold_min = 0;        // required > 0
  int enter_window_min = 10;   // > 0
  OnSignalLoss on_signal_loss = OnSignalLoss::kHoldWithStops;
  std::string arm_start = "09:00";
  std::string arm_end = "13:00";
  int arm_start_hhmm = 900;
  int arm_end_hhmm = 1300;
};

struct Scenario {
  std::string name = "scenario";
  std::string symbol;
  Market market = Market::kTse;
  Board board = Board::kOddLot;
  Side side = Side::kBuy;
  Product product = Product::kStock;  // tick schedule; caller-supplied
  std::string funding_type = "Cash";
  std::string time_in_force = "ROD";

  long budget_twd = 0;
  long budget_shares = 0;     // shares-denominated goal; XOR budget_twd (exactly one > 0)
  long position_shares = 0;   // Sell only: hard cap on cumulative sell exposure (required > 0)
  long shares_per_order = 0;  // 0 => auto fee-optimal
  Pacing pacing = Pacing::kTwap;

  FeeParams fees;

  PricePolicy price_policy = PricePolicy::kCross;
  int tick_offset = 0;
  int peg_level = 1;  // join only: 1 = best, N = (N-1) ticks deeper
  double reference_price = 0.0;
  double max_deviation_pct = 9.0;
  bool use_trial_quotes = false;

  std::string window_start = "09:00";
  std::string window_end = "13:25";
  int window_start_hhmm = 900;
  int window_end_hhmm = 1325;
  bool weekdays_only = true;

  bool require_two_sided = true;
  long quote_max_age_ms = 5000;
  long quote_stall_alert_ms = 30000;
  long ack_timeout_ms = 3000;  // un-acked order past this => local reject + re-place
  // Consecutive submit-reject / ack-timeout failures that halt the run (fail-closed).
  long max_consecutive_order_failures = 3;  // <= 0 disables the halt
  bool stop_on_disconnect = true;

  // Run-state journal dir (restart-safe fill accounting). Empty => disabled.
  std::string journal_dir;

  // [blacklist] F1 restricted-symbol safety gate. Empty path => env/default.
  std::string blacklist_path;
  int blacklist_max_stale_days = 4;
  bool blacklist_block_disposal = true;
  bool blacklist_block_attention = false;
  bool blacklist_block_margin_suspension = true;
  bool blacklist_block_sell_first = true;

  bool live = false;

  NotifyConfig notify;
  DashboardConfig dashboard;
  RoundTripConfig roundtrip;

  bool IsOddLot() const { return board == Board::kOddLot; }
};

Scenario LoadScenario(const std::string& path);  // throws std::runtime_error on parse/missing
std::vector<std::string> ValidateScenario(const Scenario& s);  // empty == valid

// Apply a --budget CLI override to s.budget_twd. A non-positive override is a
// no-op. Fails (returns false, sets *err) when the scenario is share-denominated,
// since --budget only edits the TWD knob and would otherwise trip the generic
// budget_twd/budget_shares XOR check with an unhelpful message.
bool ApplyBudgetOverride(long override_twd, Scenario* s, std::string* err);
std::string SummarizeScenario(const Scenario& s);

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SCENARIO_H_
