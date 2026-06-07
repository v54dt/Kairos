#include "scenario.h"

#include <toml++/toml.h>

#include <cstdio>
#include <format>
#include <stdexcept>

namespace kairos::exec {

namespace {

[[noreturn]] void Missing(const char* section, const char* key) {
  throw std::runtime_error(
      std::format("scenario config missing required key: {}.{}", section, key));
}

template <typename T>
T Required(const toml::table& t, const char* section, const char* key) {
  auto v = t[section][key].template value<T>();
  if (!v) Missing(section, key);
  return *v;
}

int ParseHHMM(const std::string& s) {
  int h = -1, m = -1;
  if (std::sscanf(s.c_str(), "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
    throw std::runtime_error("invalid HH:MM time: " + s);
  }
  return h * 100 + m;
}

Board ParseBoard(const std::string& s) {
  if (s == "OddLot" || s == "oddlot" || s == "零股") return Board::kOddLot;
  if (s == "RoundLot" || s == "roundlot" || s == "整股") return Board::kRoundLot;
  throw std::runtime_error("invalid board (OddLot|RoundLot): " + s);
}

Side ParseSide(const std::string& s) {
  if (s == "Buy" || s == "B" || s == "buy") return Side::kBuy;
  if (s == "Sell" || s == "S" || s == "sell") return Side::kSell;
  throw std::runtime_error("invalid side (Buy|Sell): " + s);
}

Market ParseMarket(const std::string& s) {
  if (s == "TSE" || s == "tse") return Market::kTse;
  if (s == "OTC" || s == "otc") return Market::kOtc;
  throw std::runtime_error("invalid market (TSE|OTC): " + s);
}

Pacing ParsePacing(const std::string& s) {
  if (s == "twap" || s == "TWAP") return Pacing::kTwap;
  if (s == "asap" || s == "ASAP") return Pacing::kAsap;
  throw std::runtime_error("invalid pacing (twap|asap): " + s);
}

PricePolicy ParsePolicy(const std::string& s) {
  if (s == "cross") return PricePolicy::kCross;
  if (s == "join") return PricePolicy::kJoin;
  if (s == "mid") return PricePolicy::kMid;
  if (s == "last") return PricePolicy::kLast;
  throw std::runtime_error("invalid price policy (cross|join|mid|last): " + s);
}

Product ParseProduct(const std::string& s) {
  if (s == "stock" || s == "Stock" || s == "股票") return Product::kStock;
  if (s == "etf" || s == "ETF") return Product::kEtf;
  if (s == "warrant" || s == "Warrant" || s == "權證") return Product::kWarrant;
  if (s == "cb" || s == "CB" || s == "convertible_bond" || s == "可轉債")
    return Product::kConvertibleBond;
  throw std::runtime_error("invalid product (stock|etf|warrant|cb): " + s);
}

}  // namespace

const char* BoardName(Board b) { return b == Board::kOddLot ? "OddLot" : "RoundLot"; }
const char* PacingName(Pacing p) { return p == Pacing::kTwap ? "twap" : "asap"; }
const char* SideName(Side s) { return s == Side::kBuy ? "Buy" : "Sell"; }
const char* MarketName(Market m) { return m == Market::kTse ? "TSE" : "OTC"; }
const char* PricePolicyName(PricePolicy p) {
  switch (p) {
    case PricePolicy::kCross:
      return "cross";
    case PricePolicy::kJoin:
      return "join";
    case PricePolicy::kMid:
      return "mid";
    case PricePolicy::kLast:
      return "last";
  }
  return "?";
}
const char* ProductName(Product p) {
  switch (p) {
    case Product::kStock:
      return "stock";
    case Product::kEtf:
      return "etf";
    case Product::kWarrant:
      return "warrant";
    case Product::kConvertibleBond:
      return "cb";
  }
  return "?";
}

Scenario LoadScenario(const std::string& path) {
  toml::table t;
  try {
    t = toml::parse_file(path);
  } catch (const toml::parse_error& err) {
    throw std::runtime_error(std::format("failed to parse {}: {}", path, err.description()));
  }

  Scenario s;

  // [user]
  s.creds.user_id = Required<std::string>(t, "user", "user_id");
  s.creds.password = Required<std::string>(t, "user", "password");
  s.creds.account = Required<std::string>(t, "user", "account");
  s.creds.pfx_filepath = Required<std::string>(t, "user", "pfx_filepath");
  s.creds.pfx_password = Required<std::string>(t, "user", "pfx_password");

  // [scenario]
  s.name = t["scenario"]["name"].value_or<std::string>("scenario");
  s.symbol = Required<std::string>(t, "scenario", "symbol");
  s.market = ParseMarket(t["scenario"]["market"].value_or<std::string>("TSE"));
  s.board = ParseBoard(t["scenario"]["board"].value_or<std::string>("OddLot"));
  s.side = ParseSide(t["scenario"]["side"].value_or<std::string>("Buy"));
  s.product = ParseProduct(t["scenario"]["product"].value_or<std::string>("stock"));
  s.funding_type = t["scenario"]["funding_type"].value_or<std::string>("Cash");
  s.time_in_force = t["scenario"]["time_in_force"].value_or<std::string>("ROD");
  s.budget_twd = t["scenario"]["budget_twd"].value_or<long>(0);
  s.interval_seconds = t["scenario"]["interval_seconds"].value_or<int>(30);
  s.shares_per_order = t["scenario"]["shares_per_order"].value_or<long>(0);
  s.pacing = ParsePacing(t["scenario"]["pacing"].value_or<std::string>("twap"));

  // [fees]
  s.fees.base_rate = t["fees"]["base_rate"].value_or<double>(0.001425);
  s.fees.discount = t["fees"]["discount"].value_or<double>(1.0);
  s.fees.min_fee_oddlot = t["fees"]["min_fee_oddlot"].value_or<long>(1);
  s.fees.min_fee_roundlot = t["fees"]["min_fee_roundlot"].value_or<long>(20);
  s.fees.sell_tax_rate = t["fees"]["sell_tax_rate"].value_or<double>(0.003);
  s.fees.daytrade_tax_rate = t["fees"]["daytrade_tax_rate"].value_or<double>(0.0015);
  s.fees.max_order_value_twd = t["fees"]["max_order_value_twd"].value_or<long>(0);

  // [pricing]
  s.price_policy = ParsePolicy(t["pricing"]["policy"].value_or<std::string>("cross"));
  s.tick_offset = t["pricing"]["tick_offset"].value_or<int>(0);
  s.reference_price = t["pricing"]["reference_price"].value_or<double>(0.0);
  s.max_deviation_pct = t["pricing"]["max_deviation_pct"].value_or<double>(9.0);
  s.use_trial_quotes = t["pricing"]["use_trial_quotes"].value_or<bool>(false);

  // [window]
  s.window_start = t["window"]["start_time"].value_or<std::string>("09:00");
  s.window_end = t["window"]["end_time"].value_or<std::string>("13:25");
  s.window_start_hhmm = ParseHHMM(s.window_start);
  s.window_end_hhmm = ParseHHMM(s.window_end);
  s.weekdays_only = t["window"]["weekdays_only"].value_or<bool>(true);

  // [reconnect]
  s.daily_reconnect = t["reconnect"]["daily_at"].value_or<std::string>("07:05");
  s.daily_reconnect_hhmm = ParseHHMM(s.daily_reconnect);

  // [risk]
  s.max_orders = t["risk"]["max_orders"].value_or<long>(0);
  s.max_open_orders = t["risk"]["max_open_orders"].value_or<long>(0);
  s.max_notional_twd = t["risk"]["max_notional_twd"].value_or<long>(0);
  s.require_two_sided = t["risk"]["require_two_sided"].value_or<bool>(true);
  s.quote_max_age_ms = t["risk"]["quote_max_age_ms"].value_or<long>(5000);
  s.quote_stall_alert_ms = t["risk"]["quote_stall_alert_ms"].value_or<long>(30000);
  s.stop_on_disconnect = t["risk"]["stop_on_disconnect"].value_or<bool>(true);

  // [mode]
  s.live = t["mode"]["live"].value_or<bool>(false);

  return s;
}

std::vector<std::string> ValidateScenario(const Scenario& s) {
  std::vector<std::string> errs;

  if (s.symbol.empty()) errs.push_back("scenario.symbol is empty");
  if (s.budget_twd <= 0) errs.push_back("scenario.budget_twd must be > 0");
  if (s.interval_seconds <= 0) errs.push_back("scenario.interval_seconds must be > 0");
  if (s.shares_per_order < 0) errs.push_back("scenario.shares_per_order must be >= 0");
  if (s.board == Board::kRoundLot && s.shares_per_order > 0 && s.shares_per_order % 1000 != 0) {
    errs.push_back("RoundLot shares_per_order must be a multiple of 1000");
  }
  if (s.fees.base_rate <= 0) errs.push_back("fees.base_rate must be > 0");
  if (s.fees.discount <= 0 || s.fees.discount > 1.0)
    errs.push_back("fees.discount must be in (0,1]");
  if (s.max_deviation_pct <= 0 || s.max_deviation_pct > 10.0)
    errs.push_back("pricing.max_deviation_pct must be in (0,10] (TWSE daily band is 10%)");
  if (s.window_start_hhmm >= s.window_end_hhmm)
    errs.push_back("window.start_time must be before window.end_time");
  if (s.creds.user_id.empty() || s.creds.password.empty())
    errs.push_back("user credentials are incomplete");
  if (s.creds.pfx_filepath.empty()) errs.push_back("user.pfx_filepath is empty");

  return errs;
}

std::string SummarizeScenario(const Scenario& s) {
  long cap = ResolvedMaxOrderValueTwd(s.fees, s.IsOddLot());
  std::string out;
  out += std::format("scenario: {}\n", s.name);
  out += std::format("  symbol   : {} ({}, {}, {})\n", s.symbol, MarketName(s.market),
                     BoardName(s.board), ProductName(s.product));
  out +=
      std::format("  side/cond: {} / {} / {}\n", SideName(s.side), s.funding_type, s.time_in_force);
  out += std::format("  budget   : NT$ {}  every {}s\n", s.budget_twd, s.interval_seconds);
  out += std::format("  pacing   : {}\n", PacingName(s.pacing));
  if (s.shares_per_order > 0) {
    out += std::format("  per order: {} shares (fixed)\n", s.shares_per_order);
  } else {
    out += std::format("  per order: auto (fee-optimal, <= NT$ {})\n", cap);
  }
  out += std::format("  pricing  : {} (offset {} ticks, max dev {:.1f}%)\n",
                     PricePolicyName(s.price_policy), s.tick_offset, s.max_deviation_pct);
  out += std::format("  window   : {} ~ {} {}\n", s.window_start, s.window_end,
                     s.weekdays_only ? "(weekdays)" : "");
  out += std::format("  mode     : {}\n", s.live ? "*** LIVE ***" : "PAPER");
  return out;
}

}  // namespace kairos::exec
