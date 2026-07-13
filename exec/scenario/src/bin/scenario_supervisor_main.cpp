// kairos_scenario_supervisord [--scenario-dir DIR] [--trader-bin PATH] [--ctl-sock PATH]
//   The single owner of every scenario-trader lifecycle: fork/exec-s each trader,
//   keeps the child handle (exit codes from waitpid, stop = SIGINT its own child),
//   and serves list/start/stop over a JSON-lines UDS control socket. On shutdown
//   every child is SIGINT-ed and reaped, so no orphans are left behind.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "scenario_ctl_path.h"
#include "scenario_ctl_server.h"
#include "scenario_state.h"

using namespace kairos::exec;

namespace {
std::atomic<bool> g_stop{false};
void OnSig(int) { g_stop = true; }

std::string DefaultScenarioDir() {
  if (const char* env = std::getenv("KAIROS_SCENARIO_DIR"); env != nullptr && env[0] != '\0')
    return env;
  const char* home = std::getenv("HOME");
  std::string base = (home != nullptr && home[0] != '\0') ? home : ".";
  return base + "/Kairos/exec/scenario";
}

// Optional crash-restart overrides; unset keeps the RestartPolicy defaults.
RestartPolicy RestartPolicyFromEnv() {
  RestartPolicy p;
  if (const char* v = std::getenv("KAIROS_RESTART_BASE_MS"); v != nullptr && v[0] != '\0')
    p.base_delay = std::chrono::milliseconds(std::atol(v));
  if (const char* v = std::getenv("KAIROS_RESTART_MAX_MS"); v != nullptr && v[0] != '\0')
    p.max_delay = std::chrono::milliseconds(std::atol(v));
  if (const char* v = std::getenv("KAIROS_RESTART_MAX_RETRIES"); v != nullptr && v[0] != '\0')
    p.max_retries = std::atoi(v);
  return p;
}
}  // namespace

int main(int argc, char** argv) {
  std::string scenario_dir;
  std::string trader_bin_flag;
  std::string ctl_sock_flag;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--scenario-dir" && i + 1 < argc) {
      scenario_dir = argv[++i];
    } else if (a == "--trader-bin" && i + 1 < argc) {
      trader_bin_flag = argv[++i];
    } else if (a == "--ctl-sock" && i + 1 < argc) {
      ctl_sock_flag = argv[++i];
    } else {
      std::fprintf(stderr,
                   "usage: kairos_scenario_supervisord [--scenario-dir DIR] [--trader-bin PATH] "
                   "[--ctl-sock PATH]\n");
      return 1;
    }
  }
  if (scenario_dir.empty()) scenario_dir = DefaultScenarioDir();
  std::string trader_bin =
      ResolveTraderBin(trader_bin_flag, std::getenv("KAIROS_SCENARIO_TRADER"), scenario_dir);
  std::string ctl_sock = ctl_sock_flag.empty() ? ScenarioCtlSocketPath() : ctl_sock_flag;

  std::signal(SIGINT, OnSig);
  std::signal(SIGTERM, OnSig);
  std::signal(SIGPIPE, SIG_IGN);  // a dead client must not kill the daemon on write

  Supervisor sup(scenario_dir, trader_bin, RestartPolicyFromEnv());
  ScenarioCtlServer server(&sup, ctl_sock);
  std::printf("kairos-scenario-supervisor: scenario-dir %s, trader %s\n", scenario_dir.c_str(),
              trader_bin.c_str());
  std::fflush(stdout);
  if (!server.Start()) return 1;
  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::printf("kairos-scenario-supervisor: shutting down\n");
  std::fflush(stdout);
  server.Stop();
  sup.StopAll();  // SIGINT + reap every owned trader; zero orphans
  return 0;
}
