// Integration test: the ProcessManager owns real child processes. Uses /bin/sh
// helpers to prove: stdout lines drive the state machine, a nonzero exit is a
// crash, StopChild SIGINTs and reaps to stopped, an ignore-SIGINT child is still
// reaped via SIGKILL escalation, and after StopAll no child survives (no zombie:
// waitpid -> ECHILD; no process: kill(pid,0) -> ESRCH).

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "scenario_process.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

static std::vector<std::string> Sh(const std::string& script) { return {"/bin/sh", "-c", script}; }

static bool WaitForFile(const std::string& path, int ms) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (::access(path.c_str(), F_OK) == 0) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return ::access(path.c_str(), F_OK) == 0;
}

// Wait until StatusOf(name).state satisfies `pred`, or the timeout elapses.
template <typename Pred>
static bool WaitState(const ProcessManager& pm, const std::string& name, Pred pred, int ms) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred(pm.StatusOf(name))) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred(pm.StatusOf(name));
}

int main() {
  // 1) A printer whose stdout lines drive the state machine, then a clean exit.
  {
    ProcessManager pm;
    std::string script =
        "printf 'kairos-exec: Buy 2330 NT$ 300000, cross, PAPER\\n';"
        "printf 'kairos-exec: fill k-1 1000 @ 925.00  (cum 1000 sh / NT$ 925000, fee 1318)\\n';"
        "sleep 1;"
        "printf 'kairos-exec: end - filled 1000 sh / NT$ 925000 of 300000, fee NT$ 1318\\n';"
        "exit 0";
    CHECK(pm.Spawn("printer", Sh(script), /*live=*/false));
    CHECK(WaitState(
        pm, "printer",
        [](const ProcessManager::ChildStatus& s) { return s.state == ScenarioState::kInWindow; },
        3000));
    ProcessManager::ChildStatus mid = pm.StatusOf("printer");
    CHECK(mid.counters.cum_fills == 1);
    CHECK(mid.counters.cum_shares == 1000);
    CHECK(mid.pid > 0);
    // Runs to completion -> closed-exited.
    CHECK(WaitState(
        pm, "printer",
        [](const ProcessManager::ChildStatus& s) {
          return s.state == ScenarioState::kClosedExited;
        },
        4000));
    ProcessManager::ChildStatus fin = pm.StatusOf("printer");
    CHECK(fin.last_exit_reason == "window closed / run complete");
    CHECK(fin.pid == 0);  // reaped
  }

  // 2) A fast nonzero exit -> crashed.
  {
    ProcessManager pm;
    CHECK(pm.Spawn("boom", Sh("printf 'kairos-exec: order backend connect failed\\n' 1>&2; exit 3"),
                   false));
    CHECK(WaitState(
        pm, "boom",
        [](const ProcessManager::ChildStatus& s) { return s.state == ScenarioState::kCrashed; },
        3000));
    ProcessManager::ChildStatus s = pm.StatusOf("boom");
    // Nonzero code -> crashed; the captured fatal line becomes the reason.
    CHECK(s.last_exit_reason == "order backend connect failed");
  }

  // 3) StopChild SIGINTs a sleeper (default SIGINT disposition terminates it) and
  //    reaps it to stopped.
  {
    ProcessManager pm;
    CHECK(pm.Spawn("sleeper", Sh("sleep 30"), false));
    pid_t pid = static_cast<pid_t>(pm.StatusOf("sleeper").pid);
    CHECK(pid > 0);
    pm.StopChild("sleeper");
    ProcessManager::ChildStatus s = pm.StatusOf("sleeper");
    CHECK(s.state == ScenarioState::kStopped);
    CHECK(s.last_exit_reason == "stopped by operator");
    CHECK(s.pid == 0);
    // No zombie, no survivor.
    CHECK(::kill(pid, 0) == -1 && errno == ESRCH);
  }

  // 4) A child that IGNORES SIGINT is still reaped via SIGKILL escalation. The
  //    child arms SIG_IGN and only then touches a marker file; we wait for the
  //    marker before StopChild so the SIGINT is genuinely ignored and the grace ->
  //    SIGKILL path is exercised (not a race where SIGINT kills it outright). Uses
  //    a single execv'd process so there is no shell grandchild holding the pipe.
  {
    ProcessManager pm;
    char marker[] = "/tmp/kairos-stubborn-XXXXXX";
    int mfd = ::mkstemp(marker);
    CHECK(mfd >= 0);
    ::close(mfd);
    ::unlink(marker);  // the child re-creates it once its SIGINT handler is armed
    const std::string prog =
        "import signal,sys,time\n"
        "signal.signal(signal.SIGINT, signal.SIG_IGN)\n"
        "open(sys.argv[1],'w').close()\n"
        "time.sleep(30)\n";
    CHECK(pm.Spawn("stubborn", {"/usr/bin/env", "python3", "-c", prog, marker}, false));
    pid_t pid = static_cast<pid_t>(pm.StatusOf("stubborn").pid);
    CHECK(pid > 0);
    CHECK(WaitForFile(marker, 5000));  // SIGINT handler is now armed
    auto t0 = std::chrono::steady_clock::now();
    pm.StopChild("stubborn");  // SIGINT ignored -> SIGKILL after the grace window
    auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(pm.StatusOf("stubborn").state == ScenarioState::kStopped);
    CHECK(::kill(pid, 0) == -1 && errno == ESRCH);
    // SIGINT was ignored, so StopChild must have waited the grace, then SIGKILL'd.
    CHECK(elapsed >= std::chrono::seconds(4));
    CHECK(elapsed < std::chrono::seconds(8));
    ::unlink(marker);
  }

  // 5) StopAll SIGINTs + reaps every live child, leaving no orphan.
  {
    ProcessManager pm;
    CHECK(pm.Spawn("a", Sh("sleep 30"), false));
    CHECK(pm.Spawn("b", Sh("sleep 30"), false));
    pid_t pa = static_cast<pid_t>(pm.StatusOf("a").pid);
    pid_t pb = static_cast<pid_t>(pm.StatusOf("b").pid);
    CHECK(pa > 0 && pb > 0);
    pm.StopAll();
    CHECK(::kill(pa, 0) == -1 && errno == ESRCH);
    CHECK(::kill(pb, 0) == -1 && errno == ESRCH);
    // No child of ours remains to reap.
    CHECK(::waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD);
  }

  // 6) Concurrent Spawn() of the SAME name: exactly one wins, the manager does not
  //    abort, and StopAll leaves no orphan. Regression for the check/insert race
  //    where two forks both landed in the map and destroyed a joinable monitor.
  {
    ProcessManager pm;
    constexpr int kThreads = 8;
    std::atomic<int> won{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i)
      ts.emplace_back([&] {
        if (pm.Spawn("dup", Sh("sleep 30"), false)) ++won;
      });
    for (std::thread& t : ts) t.join();
    CHECK(won.load() == 1);  // only one Spawn claims the name
    pid_t pid = static_cast<pid_t>(pm.StatusOf("dup").pid);
    CHECK(pid > 0);
    pm.StopAll();
    CHECK(::kill(pid, 0) == -1 && errno == ESRCH);
    CHECK(::waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD);  // no leaked child
  }

  // 7) The monitor forwards each child stdout line to the manager's OWN stdout,
  //    prefixed with the scenario name, so journald keeps the full trader output.
  {
    int pipefd[2];
    CHECK(::pipe(pipefd) == 0);
    ::fflush(stdout);
    int saved = ::dup(STDOUT_FILENO);
    ::dup2(pipefd[1], STDOUT_FILENO);
    {
      ProcessManager pm;
      pm.Spawn("fwd", Sh("printf 'kairos-exec: hello world\\n'; exit 0"), false);
      WaitState(
          pm, "fwd",
          [](const ProcessManager::ChildStatus& s) {
            return s.state == ScenarioState::kClosedExited;
          },
          4000);
    }  // pm destroyed here: monitor joined, all forwarding flushed
    ::fflush(stdout);
    ::dup2(saved, STDOUT_FILENO);  // restore the real stdout
    ::close(saved);
    ::close(pipefd[1]);
    std::string out;
    char b[512];
    ssize_t n;
    while ((n = ::read(pipefd[0], b, sizeof(b))) > 0) out.append(b, static_cast<std::size_t>(n));
    ::close(pipefd[0]);
    CHECK(out.find("[fwd] kairos-exec: hello world") != std::string::npos);
  }

  if (g_failures == 0) {
    std::printf("test_scenario_process: OK\n");
    return 0;
  }
  std::printf("test_scenario_process: FAILED %d check(s)\n", g_failures);
  return 1;
}
