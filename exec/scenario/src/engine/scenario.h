#ifndef KAIROS_EXEC_SCENARIO_H_
#define KAIROS_EXEC_SCENARIO_H_

// Declarative scenario (劇本) config consumed by the execution engine. The TOML
// loader / validator / summary live in scenario.cpp.

#include <string>
#include <vector>

#include "dashboard_metrics.h"
#include "notify_config.h"
#include "tw_fees.h"
#include "tw_market.h"

namespace kairos::exec {

enum class Board { kOddLot, kRoundLot };
enum class Side { kBuy, kSell };
enum class Market { kTse, kOtc };
enum class Pacing { kAsap, kTwap };
enum class PricePolicy { kCross, kJoin, kMid, kLast };

struct UserCreds {
  std::string user_id;
  std::string password;
  std::string account;
  std::string pfx_filepath;
  std::string pfx_password;
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

  bool IsOddLot() const { return board == Board::kOddLot; }
};

Scenario LoadScenario(const std::string& path);  // throws std::runtime_error on parse/missing
std::vector<std::string> ValidateScenario(const Scenario& s);  // empty == valid
std::string SummarizeScenario(const Scenario& s);

const char* BoardName(Board b);
const char* PacingName(Pacing p);
const char* SideName(Side s);
const char* MarketName(Market m);
const char* PricePolicyName(PricePolicy p);
const char* ProductName(Product p);

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SCENARIO_H_
