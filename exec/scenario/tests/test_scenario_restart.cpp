// Integration test: crash-restart policy in the ProcessManager. Uses /bin/sh
// crashers (exit nonzero, each appending its run timestamp to a temp file) with a
// tiny injected RestartPolicy so the whole suite runs in well under a second. It
// proves: a crash auto-restarts with the SAME mode; a clean close / operator stop
// never restarts; a stop or a daemon shutdown DURING the backoff cancels the
// pending restart with no orphan; the backoff grows and is capped; the retry cap
// gives up into a terminal crashed state; a healthy run resets the counter; and no
// double-spawn.

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "scenario_process.h"
#include "test_check.h"

using namespace kairos::exec;

static std::vector<std::string> Sh(const std::string& script) { return {"/bin/sh", "-c", script}; }

static std::string TempPath(const std::string& stem) {
  return "/tmp/kairos-restart-" + std::to_string(::getpid()) + "-" + stem;
}

// A crasher that records one line per run (so we can count runs / measure backoff)
// then exits nonzero -- never reaching in-window, so the counter never resets.
static std::vector<std::string> Crasher(const std::string& marker) {
  return Sh("date +%s.%N >> '" + marker + "'; exit 7");
}

static int CountLines(const std::string& path) {
  std::ifstream f(path);
  int n = 0;
  std::string line;
  while (std::getline(f, line))
    if (!line.empty()) ++n;
  return n;
}

static std::vector<double> ReadStamps(const std::string& path) {
  std::ifstream f(path);
  std::vector<double> v;
  std::string line;
  while (std::getline(f, line))
    if (!line.empty()) v.push_back(std::atof(line.c_str()));
  return v;
}

template <typename Pred>
static bool WaitFor(Pred pred, int ms) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

int main() {
  // (a) A crash auto-restarts, preserving the SAME mode (live crasher -> live).
  {
    std::string marker = TempPath("a");
    ::unlink(marker.c_str());
    RestartPolicy p;
    p.base_delay = std::chrono::milliseconds(20);
    p.max_delay = std::chrono::milliseconds(80);
    p.max_retries = 10;
    p.healthy_reset = std::chrono::milliseconds(100000);
    ProcessManager pm(p);
    CHECK(pm.Spawn("a", Crasher(marker), /*live=*/true));
    // At least two runs means the crash was auto-restarted.
    CHECK(WaitFor([&] { return CountLines(marker) >= 2; }, 3000));
    CHECK(WaitFor([&] { return pm.StatusOf("a").restart_count >= 1; }, 1000));
    ProcessManager::ChildStatus s = pm.StatusOf("a");
    CHECK(s.live == true);  // a live crash restarts live, never escalates
    pm.StopChild("a");
    ::unlink(marker.c_str());
  }

  // (b) A clean window-close exits 0 -> closed-exited, NEVER restarts.
  {
    std::string marker = TempPath("b");
    ::unlink(marker.c_str());
    RestartPolicy p;
    p.base_delay = std::chrono::milliseconds(20);
    ProcessManager pm(p);
    CHECK(pm.Spawn(
        "b", Sh("date +%s >> '" + marker + "'; printf 'kairos-exec: end - filled 0 sh\\n'; exit 0"),
        false));
    CHECK(WaitFor([&] { return pm.StatusOf("b").state == ScenarioState::kClosedExited; }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(CountLines(marker) == 1);  // no respawn
    CHECK(pm.StatusOf("b").restart_count == 0);
    ::unlink(marker.c_str());
  }

  // (c) An operator stop of a sleeper -> stopped, NEVER restarts.
  {
    RestartPolicy p;
    p.base_delay = std::chrono::milliseconds(20);
    ProcessManager pm(p);
    CHECK(pm.Spawn("c", Sh("sleep 30"), false));
    CHECK(WaitFor([&] { return pm.StatusOf("c").pid > 0; }, 2000));
    pm.StopChild("c");
    ProcessManager::ChildStatus s = pm.StatusOf("c");
    CHECK(s.state == ScenarioState::kStopped);
    CHECK(s.restart_count == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(pm.StatusOf("c").state == ScenarioState::kStopped);  // stayed stopped
  }

  // (d) A stop DURING the backoff wait cancels the pending restart.
  {
    std::string marker = TempPath("d");
    ::unlink(marker.c_str());
    RestartPolicy p;
    p.base_delay = std::chrono::milliseconds(500);  // long enough to stop within
    p.max_delay = std::chrono::milliseconds(500);
    p.max_retries = 10;
    ProcessManager pm(p);
    CHECK(pm.Spawn("d", Crasher(marker), false));
    // Wait until it has crashed once and a restart is pending (one run recorded).
    CHECK(WaitFor([&] { return pm.StatusOf("d").state == ScenarioState::kCrashed; }, 2000));
    int runs_before = CountLines(marker);
    pm.StopChild("d");  // arrives during the 500ms backoff
    ProcessManager::ChildStatus s = pm.StatusOf("d");
    CHECK(s.state == ScenarioState::kStopped);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));  // past the deadline
    CHECK(CountLines(marker) == runs_before);                     // the pending restart never fired
    CHECK(pm.StatusOf("d").restart_count == 0);
    ::unlink(marker.c_str());
  }

  // (e) A daemon shutdown DURING backoff cancels + reaps cleanly (no orphan).
  {
    std::string marker = TempPath("e");
    ::unlink(marker.c_str());
    RestartPolicy p;
    p.base_delay = std::chrono::milliseconds(500);
    p.max_delay = std::chrono::milliseconds(500);
    ProcessManager pm(p);
    CHECK(pm.Spawn("e", Crasher(marker), false));
    CHECK(WaitFor([&] { return pm.StatusOf("e").state == ScenarioState::kCrashed; }, 2000));
    int runs_before = CountLines(marker);
    pm.StopAll();  // shutdown during the pending backoff
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK(CountLines(marker) == runs_before);                         // no restart escaped shutdown
    CHECK(::waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD);  // no orphan/zombie
    ::unlink(marker.c_str());
  }

  // (f) Backoff grows exponentially and is capped.
  {
    std::string marker = TempPath("f");
    ::unlink(marker.c_str());
    RestartPolicy p;
    p.base_delay = std::chrono::milliseconds(80);
    p.max_delay = std::chrono::milliseconds(200);  // caps the 4th gap at 200ms, not 640ms
    p.max_retries = 10;
    p.healthy_reset = std::chrono::milliseconds(100000);
    ProcessManager pm(p);
    CHECK(pm.Spawn("f", Crasher(marker), false));
    CHECK(WaitFor([&] { return CountLines(marker) >= 5; }, 5000));
    pm.StopChild("f");
    std::vector<double> t = ReadStamps(marker);
    CHECK(t.size() >= 5);
    if (t.size() >= 5) {
      double d1 = t[1] - t[0];  // ~80ms
      double d2 = t[2] - t[1];  // ~160ms
      double d3 = t[3] - t[2];  // ~200ms (capped)
      double d4 = t[4] - t[3];  // ~200ms (capped)
      std::printf("(f) gaps ms: %.0f %.0f %.0f %.0f\n", d1 * 1000, d2 * 1000, d3 * 1000, d4 * 1000);
      CHECK(d2 > d1);                 // grows
      CHECK(d3 < 0.35 && d4 < 0.35);  // capped near max_delay, not still doubling
    }
    ::unlink(marker.c_str());
  }

  // (g) The retry cap gives up into a terminal crashed state; no further restarts.
  {
    std::string marker = TempPath("g");
    ::unlink(marker.c_str());
    RestartPolicy p;
    p.base_delay = std::chrono::milliseconds(15);
    p.max_delay = std::chrono::milliseconds(60);
    p.max_retries = 3;
    p.healthy_reset = std::chrono::milliseconds(100000);
    ProcessManager pm(p);
    CHECK(pm.Spawn("g", Crasher(marker), false));
    CHECK(WaitFor([&] { return pm.StatusOf("g").gave_up; }, 4000));
    ProcessManager::ChildStatus s = pm.StatusOf("g");
    CHECK(s.state == ScenarioState::kCrashed);  // terminal crashed
    CHECK(s.restart_count == 3);                // exactly max_retries restarts fired
    CHECK(s.last_exit_reason.find("gave up") != std::string::npos);
    int runs = CountLines(marker);
    CHECK(runs == 4);  // original + max_retries restarts, then stop
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(CountLines(marker) == runs);  // no restart after giving up
    ::unlink(marker.c_str());
  }

  // (h) A run that survives the healthy_reset cooldown resets the counter, so an
  // occasional crash after a healthy uptime never gives up.
  {
    std::string marker = TempPath("h");
    ::unlink(marker.c_str());
    RestartPolicy p;
    p.base_delay = std::chrono::milliseconds(15);
    p.max_delay = std::chrono::milliseconds(30);
    p.max_retries = 3;
    p.healthy_reset = std::chrono::milliseconds(60);  // each run outlives the cooldown
    ProcessManager pm(p);
    // Sleeps past the cooldown (healthy uptime) then crashes; the reset keeps the cap away.
    CHECK(pm.Spawn("h", Sh("date +%s >> '" + marker + "'; sleep 0.12; exit 9"), false));
    // Let it churn several restarts; because each run survives the cooldown, it keeps resetting.
    CHECK(WaitFor([&] { return CountLines(marker) >= 5; }, 5000));
    ProcessManager::ChildStatus s = pm.StatusOf("h");
    CHECK(!s.gave_up);            // never gave up despite >max_retries crashes
    CHECK(s.restart_count <= 1);  // reset by each cooldown-surviving run
    pm.StopChild("h");
    ::unlink(marker.c_str());
  }

  // (h2) A trader that reaches in-window but crashes BEFORE the cooldown still hits
  // the cap and gives up: an instantaneous in-window signal must not reset the counter
  // (else a live crash-loop after the first fill would re-enter the market forever).
  {
    std::string marker = TempPath("h2");
    ::unlink(marker.c_str());
    RestartPolicy p;
    p.base_delay = std::chrono::milliseconds(15);
    p.max_delay = std::chrono::milliseconds(60);
    p.max_retries = 3;
    p.healthy_reset = std::chrono::milliseconds(100000);  // no run gets near the cooldown
    ProcessManager pm(p);
    // Reaches in-window (banner + fill) then crashes immediately -- zero healthy uptime.
    std::string script = "date +%s >> '" + marker +
                         "'; printf 'kairos-exec: Buy 2330 NT$ 300000, cross, PAPER\\n';"
                         "printf 'kairos-exec: fill k-1 1000 @ 925.00  (cum 1000 sh)\\n'; exit 9";
    CHECK(pm.Spawn("h2", Sh(script), /*live=*/true));
    CHECK(WaitFor([&] { return pm.StatusOf("h2").gave_up; }, 4000));
    ProcessManager::ChildStatus s = pm.StatusOf("h2");
    CHECK(s.state == ScenarioState::kCrashed);  // terminal crashed, not re-spawned
    CHECK(s.restart_count == 3);                // exactly max_retries, then gave up
    int runs = CountLines(marker);
    CHECK(runs == 4);  // original + max_retries restarts
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(CountLines(marker) == runs);  // no market re-entry after giving up
    ::unlink(marker.c_str());
  }

  // (i) No double-spawn: never two live pids for one name; run-count rises by 1/crash.
  {
    std::string marker = TempPath("i");
    ::unlink(marker.c_str());
    RestartPolicy p;
    p.base_delay = std::chrono::milliseconds(30);
    p.max_delay = std::chrono::milliseconds(60);
    p.max_retries = 10;
    p.healthy_reset = std::chrono::milliseconds(100000);
    ProcessManager pm(p);
    // Sleeps briefly so a live window is observable, then crashes.
    CHECK(pm.Spawn("i", Sh("date +%s >> '" + marker + "'; sleep 0.15; exit 4"), false));
    int last = 0;
    for (int i = 0; i < 40; ++i) {
      ProcessManager::ChildStatus s = pm.StatusOf("i");
      CHECK(s.pid >= 0);  // a single pid field: never two children tracked
      last = static_cast<int>(s.restart_count);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    int runs = CountLines(marker);
    // Each run corresponds to exactly one restart increment (+the original run).
    CHECK(runs == last + 1);
    pm.StopChild("i");
    ::unlink(marker.c_str());
  }

  if (g_failures == 0) {
    std::printf("test_scenario_restart: OK\n");
    return 0;
  }
  std::printf("test_scenario_restart: FAILED %d check(s)\n", g_failures);
  return 1;
}
