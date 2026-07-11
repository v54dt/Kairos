// kairos_scenario_trader <scenario.toml> [mode] [--budget n] [--ignore-window] [--yes]
//   (default)        PAPER: real quotes, queue-model simulated fills, no order socket
//   --paper-instant  PAPER: real quotes, instant full fills (pipeline sanity mode)
//   --check          offline: print the plan, no connection
//   --quotes [secs]  quote-only: print the live quote for `secs`, no orders
//   --live           real orders (requires typing LIVE, or --yes to skip)

#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "blacklist.h"
#include "dashboard_metrics.h"
#include "engine.h"
#include "event_sink.h"
#include "http_poster.h"
#include "hub_order_backend.h"
#include "ntfy_dispatcher.h"
#include "order_backend.h"
#include "queue_sim_backend.h"
#include "scenario.h"
#include "socket_path.h"
#include "tw_fees.h"
#include "tw_market.h"
#include "uds_quote_client.h"

using namespace kairos::exec;

namespace {
ScenarioEngine* g_engine = nullptr;
std::atomic<bool> g_stop{false};
void OnSig(int) {
  g_stop = true;
  if (g_engine) g_engine->RequestStop();
}

void PrintPlan(const Scenario& s) {
  std::printf("%s", SummarizeScenario(s).c_str());
  if (s.reference_price > 0) {
    Cents px = RoundNearestTick(FloatToCents(s.reference_price), s.product);
    long shares = s.shares_per_order > 0 ? s.shares_per_order
                                         : OptimalSharesPerOrder(px, s.fees, s.IsOddLot());
    Cents notional = px * shares;
    long fee = BrokerageFee(notional, s.fees, s.IsOddLot());
    long orders = notional > 0 ? (s.budget_twd + notional / 100 - 1) / (notional / 100) : 0;
    std::printf("  plan@%s: %ld sh/order, NT$ %ld/order, fee %ld; ~%ld orders, ~NT$ %ld fees\n",
                CentsToString(px).c_str(), shares, notional / 100, fee, orders, orders * fee);
  } else {
    std::printf("  (pass reference_price to preview the slice plan)\n");
  }
}

int RunQuotes(const Scenario& s, int secs) {
  std::atomic<bool> got{false};
  UdsQuoteClient client(
      QuoteSocketPath(), {s.symbol}, [&](const std::string& sym, const TopOfBook& t) {
        got = true;
        std::printf("%-8s bid=%s ask=%s last=%s%s\n", sym.c_str(),
                    CentsToString(t.best_bid()).c_str(), CentsToString(t.best_ask()).c_str(),
                    CentsToString(t.last_trade).c_str(), t.is_trial ? " (trial)" : "");
        std::fflush(stdout);
      });
  client.Start();
  for (int i = 0; i < secs * 10 && !g_stop; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  client.Stop();
  std::printf(got ? "kairos-exec: quotes OK\n" : "kairos-exec: NO quotes received\n");
  return got ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
  std::string path;
  bool flag_check = false, flag_quotes = false, flag_live = false, assume_yes = false;
  bool flag_paper_instant = false;
  bool ignore_window = false;
  bool ignore_blacklist = false;
  int quotes_secs = 30;
  long override_budget = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--check") {
      flag_check = true;
    } else if (a == "--quotes") {
      flag_quotes = true;
      if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0])))
        quotes_secs = std::atoi(argv[++i]);
    } else if (a == "--live") {
      flag_live = true;
    } else if (a == "--paper-instant") {
      flag_paper_instant = true;
    } else if (a == "--yes" || a == "-y") {
      assume_yes = true;
    } else if (a == "--ignore-window") {
      ignore_window = true;
    } else if (a == "--ignore-blacklist") {
      ignore_blacklist = true;
    } else if (a == "--budget" && i + 1 < argc) {
      override_budget = std::atol(argv[++i]);
    } else if (!a.empty() && a[0] != '-') {
      path = a;
    }
  }
  if (path.empty()) {
    std::fprintf(stderr,
                 "usage: kairos_scenario_trader <scenario.toml> "
                 "[--check|--quotes [s]|--live|--paper-instant] "
                 "[--budget n] [--ignore-window] [--ignore-blacklist] [--yes]\n");
    return 1;
  }

  Scenario scenario;
  try {
    scenario = LoadScenario(path);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "failed to load scenario: %s\n", e.what());
    return 1;
  }
  std::string budget_err;
  if (!ApplyBudgetOverride(override_budget, &scenario, &budget_err)) {
    std::fprintf(stderr, "%s\n", budget_err.c_str());
    return 1;
  }
  scenario.live = flag_live;

  auto errs = ValidateScenario(scenario);
  if (!errs.empty()) {
    std::fprintf(stderr, "scenario invalid:\n");
    for (const auto& e : errs) std::fprintf(stderr, "  - %s\n", e.c_str());
    return 1;
  }

  {
    BlacklistConfig bl_cfg;
    bl_cfg.path = scenario.blacklist_path;
    bl_cfg.max_stale_days = scenario.blacklist_max_stale_days;
    bl_cfg.block_disposal = scenario.blacklist_block_disposal;
    bl_cfg.block_attention = scenario.blacklist_block_attention;
    bl_cfg.block_margin_suspension = scenario.blacklist_block_margin_suspension;
    bl_cfg.block_sell_first = scenario.blacklist_block_sell_first;
    std::string bl_path = ResolveBlacklistPath(bl_cfg.path);
    BlacklistGateOutcome outcome =
        EvaluateBlacklistGate(bl_path, bl_cfg, scenario.symbol, std::time(nullptr));
    if (outcome.has_warning) {
      std::fprintf(stderr, "WARNING: %s\n", outcome.message.c_str());
    }
    if (outcome.result == BlacklistGateResult::kRefuse) {
      if (BlacklistOverride(ignore_blacklist, assume_yes)) {
        std::fprintf(stderr, "*** BLACKLIST GATE OVERRIDDEN (--ignore-blacklist --yes) ***\n%s\n",
                     outcome.message.c_str());
      } else {
        std::fprintf(stderr, "REFUSING TO TRADE: %s\n", outcome.message.c_str());
        if (ignore_blacklist && !assume_yes)
          std::fprintf(stderr, "(--ignore-blacklist also requires --yes to override)\n");
        return 1;
      }
    } else {
      std::printf("%s\n", outcome.message.c_str());
    }
  }

  std::signal(SIGINT, OnSig);
  std::signal(SIGTERM, OnSig);

  if (flag_check) {
    PrintPlan(scenario);
    std::printf("kairos-exec: config OK (offline check)\n");
    return 0;
  }
  if (flag_quotes) {
    return RunQuotes(scenario, quotes_secs);
  }

  std::printf("%s", SummarizeScenario(scenario).c_str());
  std::fflush(stdout);

  // --live routes orders through the shared order hub (kairos_order_hubd); the hub
  // holds the account creds and the 1 req/s gate, so the scenario just connects.
  // Default paper = queue-model sim (realistic passive fills); --paper-instant
  // keeps the old instant-full-fill backend for pipeline sanity checks.
  std::unique_ptr<OrderBackend> backend_owned;
  if (scenario.live) {
    if (!assume_yes) {
      std::printf("*** LIVE via order hub: %s %s NT$ %ld. Type LIVE to confirm: ",
                  SideName(scenario.side), scenario.symbol.c_str(), scenario.budget_twd);
      std::fflush(stdout);
      std::string line;
      std::getline(std::cin, line);
      if (line != "LIVE") {
        std::printf("cancelled\n");
        return 0;
      }
    }
    backend_owned = std::make_unique<HubOrderBackend>(OrderSocketPath());
  } else if (flag_paper_instant) {
    backend_owned = std::make_unique<PaperOrderBackend>();
  } else {
    backend_owned = std::make_unique<QueueSimBackend>(FillMode::kProbQueue,
                                                      std::vector<std::string>{scenario.symbol});
  }
  OrderBackend* backend = backend_owned.get();

  std::unique_ptr<HttpPoster> poster;
  std::unique_ptr<NtfyDispatcher> dispatcher;
  std::unique_ptr<DashboardMetrics> dashboard;
  NullEventSink null_sink;
  EventSink* sink = &null_sink;
  if (scenario.notify.enabled || scenario.dashboard.enabled) {
    poster = std::make_unique<HttpPoster>();
  }
  if (scenario.notify.enabled) {
    dispatcher = std::make_unique<NtfyDispatcher>(scenario.notify, poster.get());
    sink = dispatcher.get();
  }
  if (scenario.dashboard.enabled) {
    dashboard = std::make_unique<DashboardMetrics>(scenario.dashboard, poster.get());
  }

  UdsQuoteClient quotes(QuoteSocketPath(), {scenario.symbol});
  ScenarioEngine engine(std::move(scenario), backend, sink, &quotes);
  if (ignore_window) engine.set_ignore_window(true);
  if (dashboard) engine.set_dashboard(dashboard.get());
  g_engine = &engine;
  return engine.Run();
}
