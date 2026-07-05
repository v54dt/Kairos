#ifndef KAIROS_EXEC_SCENARIO_CTL_SERVER_H_
#define KAIROS_EXEC_SCENARIO_CTL_SERVER_H_

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "scenario_ctl_proto.h"
#include "scenario_process.h"

namespace kairos::exec {

// The single owner of the scenario-trader set: enumerates the scenario dir for
// the startable names and owns the running children via a ProcessManager. It
// answers list/start/stop into a status snapshot. Fail-closed: start requires an
// explicit, valid mode; only mode=live ever routes to a --live trader.
class Supervisor {
 public:
  Supervisor(std::string scenario_dir, std::string trader_bin);

  // Parse one JSON-lines request and dispatch it; returns the JSON response line
  // (already newline-terminated). A malformed/rejected request yields an ok=false
  // response and never spawns. Never throws.
  std::string HandleLine(const std::string& line);

  // SIGINT + reap every owned child (daemon shutdown path).
  void StopAll();

 private:
  std::string Handle(const ScenarioRequest& req);
  std::vector<ScenarioSnapshotRow> Snapshot();

  std::string scenario_dir_;
  std::string trader_bin_;
  ProcessManager pm_;
};

// UDS front end for the supervisor: mirrors OrderHubServer (accept thread +
// detached per-client threads, idempotent Stop), but frames requests as JSON
// lines. Oversized/partial/malformed input is answered with an error and never
// crashes the daemon.
class ScenarioCtlServer {
 public:
  ScenarioCtlServer(Supervisor* sup, std::string path);
  ~ScenarioCtlServer();

  bool Start();
  void Stop();

 private:
  void AcceptLoop();
  void ClientLoop(int fd);

  Supervisor* sup_;
  std::string path_;
  int listen_fd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread accept_thread_;
  std::mutex clients_mu_;
  std::unordered_set<int> live_;
  std::atomic<int> active_clients_{0};
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SCENARIO_CTL_SERVER_H_
