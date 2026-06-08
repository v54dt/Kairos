// kairos_scenario_trader <scenario.toml> [mode] [--budget n] [--ignore-window] [--yes]
//   (default)        PAPER: real quotes, simulated fills, no order socket
//   --check          offline: print the plan, no connection
//   --quotes [secs]  quote-only: print the live quote for `secs`, no orders
//   --live           real orders (requires typing LIVE, or --yes to skip)

#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "engine.h"
#include "event_sink.h"
#include "http_poster.h"
#include "live_backend.h"
#include "ntfy_dispatcher.h"
#include "order_backend.h"
#include "scenario.h"
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
                    CentsToString(t.best_bid).c_str(), CentsToString(t.best_ask).c_str(),
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
  bool ignore_window = false;
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
    } else if (a == "--yes" || a == "-y") {
      assume_yes = true;
    } else if (a == "--ignore-window") {
      ignore_window = true;
    } else if (a == "--budget" && i + 1 < argc) {
      override_budget = std::atol(argv[++i]);
    } else if (!a.empty() && a[0] != '-') {
      path = a;
    }
  }
  if (path.empty()) {
    std::fprintf(stderr,
                 "usage: kairos_scenario_trader <scenario.toml> [--check|--quotes [s]|--live] "
                 "[--budget n] [--ignore-window] [--yes]\n");
    return 1;
  }

  Scenario scenario;
  try {
    scenario = LoadScenario(path);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "failed to load scenario: %s\n", e.what());
    return 1;
  }
  if (override_budget > 0) scenario.budget_twd = override_budget;
  scenario.live = flag_live;

  auto errs = ValidateScenario(scenario);
  if (!errs.empty()) {
    std::fprintf(stderr, "scenario invalid:\n");
    for (const auto& e : errs) std::fprintf(stderr, "  - %s\n", e.c_str());
    return 1;
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

  PaperOrderBackend paper;
  std::unique_ptr<OrderBackend> live;
  OrderBackend* backend = &paper;
  if (scenario.live) {
    live = MakeLiveBackend(scenario);
    if (!live) {
      std::fprintf(stderr, "kairos-exec: this build has no broker SDK; --live unavailable\n");
      return 1;
    }
    if (!assume_yes) {
      std::printf("*** LIVE: real orders on account %s, %s %s NT$ %ld. Type LIVE to confirm: ",
                  scenario.creds.account.c_str(), SideName(scenario.side), scenario.symbol.c_str(),
                  scenario.budget_twd);
      std::fflush(stdout);
      std::string line;
      std::getline(std::cin, line);
      if (line != "LIVE") {
        std::printf("cancelled\n");
        return 0;
      }
    }
    backend = live.get();
  }

  std::unique_ptr<HttpPoster> poster;
  std::unique_ptr<NtfyDispatcher> dispatcher;
  NullEventSink null_sink;
  EventSink* sink = &null_sink;
  if (scenario.notify.enabled) {
    poster = std::make_unique<HttpPoster>();
    dispatcher = std::make_unique<NtfyDispatcher>(scenario.notify, poster.get());
    sink = dispatcher.get();
  }

  ScenarioEngine engine(std::move(scenario), backend, sink);
  if (ignore_window) engine.set_ignore_window(true);
  g_engine = &engine;
  engine.Run();
  return 0;
}
