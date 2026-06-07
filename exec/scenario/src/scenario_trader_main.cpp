// kairos_scenario_trader <scenario.toml> [--ignore-window] [--budget <twd>]
// PAPER runner (no order socket). Full modes (--check/--quotes/--live/--simfeed)
// come in C3; this is the C2b paper runner + live-test vehicle.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "engine.h"
#include "order_backend.h"
#include "scenario.h"

using namespace kairos::exec;

namespace {
ScenarioEngine* g_engine = nullptr;
void OnSig(int) {
  if (g_engine) g_engine->RequestStop();
}
}  // namespace

int main(int argc, char** argv) {
  std::string path;
  bool ignore_window = false;
  long override_budget = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--ignore-window") {
      ignore_window = true;
    } else if (a == "--budget" && i + 1 < argc) {
      override_budget = std::atol(argv[++i]);
    } else if (!a.empty() && a[0] != '-') {
      path = a;
    }
  }
  if (path.empty()) {
    std::fprintf(
        stderr,
        "usage: kairos_scenario_trader <scenario.toml> [--ignore-window] [--budget <twd>]\n");
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
  scenario.live = false;  // C2b is PAPER only

  auto errs = ValidateScenario(scenario);
  if (!errs.empty()) {
    std::fprintf(stderr, "scenario invalid:\n");
    for (const auto& e : errs) std::fprintf(stderr, "  - %s\n", e.c_str());
    return 1;
  }

  std::printf("%s", SummarizeScenario(scenario).c_str());
  std::fflush(stdout);

  PaperOrderBackend backend;
  ScenarioEngine engine(std::move(scenario), &backend);
  if (ignore_window) engine.set_ignore_window(true);
  g_engine = &engine;
  std::signal(SIGINT, OnSig);
  std::signal(SIGTERM, OnSig);
  engine.Run();
  return 0;
}
