#ifndef KAIROS_EXEC_SCENARIO_PROCESS_H_
#define KAIROS_EXEC_SCENARIO_PROCESS_H_

#include <sys/types.h>

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "scenario_state.h"

namespace kairos::exec {

// Owns every scenario-trader child: one fork+execv per trader (NO setsid/double-
// fork, so each trader is a direct child in the daemon's process group), one pipe
// per child (merged stdout+stderr), and one monitor thread per child that drives
// the state machine from the captured output and blocking-waitpid's the child on
// EOF. The daemon holds real handles, so exit codes come from waitpid and a stop
// is a SIGINT to its OWN child -- no /proc scan, no pid-reuse window, no zombies.
class ProcessManager {
 public:
  ProcessManager() = default;
  ~ProcessManager();

  ProcessManager(const ProcessManager&) = delete;
  ProcessManager& operator=(const ProcessManager&) = delete;

  // Fork+exec `argv` (argv[0] is the binary). Refuses if a child for `name` is
  // still alive; a prior terminal child for the same name is joined and replaced.
  // Returns false if the fork/exec plumbing fails.
  bool Spawn(const std::string& name, const std::vector<std::string>& argv, bool live);

  // SIGINT the named child, wait bounded for the monitor to reap, then SIGKILL if
  // it ignored the SIGINT. No-op if there is no live child for `name`.
  void StopChild(const std::string& name);

  // SIGINT every live child, bounded-wait, SIGKILL stragglers, join all monitors.
  // Leaves zero orphans: every child is a direct waitpid'd child of this process.
  void StopAll();

  // Immutable view of one owned child (running or last-terminal). `present` is
  // false when no child for `name` has ever been spawned.
  struct ChildStatus {
    bool present = false;
    ScenarioState state = ScenarioState::kStopped;
    long pid = 0;
    ScenarioCounters counters;
    std::string last_exit_reason;
    bool live = false;
  };
  ChildStatus StatusOf(const std::string& name) const;
  std::vector<std::string> Names() const;

 private:
  struct Child {
    std::string name;
    pid_t pid = -1;
    int read_fd = -1;
    bool live = false;
    bool requested_stop = false;
    bool saw_end_line = false;
    bool reaped = false;
    ScenarioState state = ScenarioState::kStarting;
    ScenarioCounters counters;
    std::string last_fail_reason;
    std::string last_exit_reason;
    std::thread monitor;
  };

  void MonitorLoop(Child* c);
  static bool IsAlive(const Child& c);

  mutable std::mutex mu_;
  std::condition_variable exit_cv_;  // fired whenever a child is reaped
  std::map<std::string, std::unique_ptr<Child>> children_;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SCENARIO_PROCESS_H_
