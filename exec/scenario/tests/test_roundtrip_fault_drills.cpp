// Round-trip fault drills: replay past incident classes end-to-end against the REAL
// binaries — the kairos-sim pipeline (quotes + a fault-configured kairos_sim_hubd),
// a real kairos_signald (manual predicate = the operator trigger), and a real
// kairos_scenario_trader in [roundtrip] mode. Each drill asserts the SAFETY OUTCOME
// (the position stays protected / fails loud, never silently unwatched), not just
// survival. Sibling to test_sim_fault_drills.cpp; same env gate, pgid hygiene and
// KAIROS_SIM_HUBD_ARGS fault passthrough, extended with the signal daemon.
//
//   DRILL 1  ENTER ack-drop storm (the 7/6 class): the enter leg halts fail-closed
//            after consecutive ack timeouts, the partial fill is transferred into
//            HOLD, and the stop watchdog still fires (exit_trigger). Bounded submits.
//   DRILL 2  HOLD signald kill: (A) hold_with_stops keeps the position and a stop
//            still exits with the daemon dead; (B) on_signal_loss=exit runs a clean
//            exit leg. Protection is independent of the signal daemon.
//   DRILL 3  EXIT reject storm (via PR5 recovery): a clean paper enter, then a
//            restarted live trader hits a reject storm on the exit leg => FATAL with
//            the remaining shares named, non-zero exit, never a silent flat.
//   DRILL 4  HOLD quote-socket break: the feed dies mid-HOLD => the staleness hard
//            stop forces an exit; the position is never left silently unwatched.
//   DRILL 5  (A) EXIT hub disconnect => the leg stops fail-closed (stop_on_disconnect)
//            with the position remaining; (B) SIGKILL the trader mid-HOLD, restart it,
//            and the PR5 recovery resumes protection and exits (the CI kill -9).
//   DRILL 6  Degenerate late trigger: a manual signal after the 13:25 arithmetic is
//            dropped with no leg started (closes the PR3 handoff loop).
//
// The wall clock is offset into the trading session with the KAIROS_RT_WALL_HHMM test
// seam so these run off-hours. Opt-in: self-skips (exit 0) unless KAIROS_FAULT_DRILLS=1.

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string RepoRoot() {
  std::filesystem::path here = __FILE__;  // <root>/exec/scenario/tests/
  return here.parent_path().parent_path().parent_path().parent_path().string();
}

bool Exists(const std::string& p) { return std::filesystem::exists(p); }

pid_t SpawnLogged(const std::vector<std::string>& argv,
                  const std::vector<std::pair<std::string, std::string>>& env,
                  const std::string& log_path, bool own_pgroup) {
  pid_t pid = ::fork();
  if (pid != 0) return pid;
  if (own_pgroup) ::setpgid(0, 0);
  for (const auto& [k, v] : env) ::setenv(k.c_str(), v.c_str(), 1);
  int fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    ::dup2(fd, STDOUT_FILENO);
    ::dup2(fd, STDERR_FILENO);
    ::close(fd);
  }
  std::vector<char*> cargv;
  for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
  cargv.push_back(nullptr);
  ::execv(cargv[0], cargv.data());
  ::_exit(127);
}

bool WaitForPath(const std::string& p, int timeout_ms) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (Exists(p)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return Exists(p);
}

int WaitExit(pid_t pid, int timeout_ms) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    int st = 0;
    pid_t r = ::waitpid(pid, &st, WNOHANG);
    if (r == pid)
      return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ::kill(pid, SIGKILL);
  ::waitpid(pid, nullptr, 0);
  return -1;
}

void KillReap(pid_t pid, int sig) {
  if (pid <= 0) return;
  ::kill(pid, sig);
  ::waitpid(pid, nullptr, 0);
}

std::string Slurp(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int Count(const std::string& hay, const std::string& needle) {
  int n = 0;
  for (std::size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
  return n;
}

// The one journal file in `dir` whose name carries `infix` (e.g. "-rt-", "-Buy-").
std::string FindJournal(const std::string& dir, const std::string& infix) {
  if (!Exists(dir)) return "";
  for (const auto& e : std::filesystem::directory_iterator(dir))
    if (e.path().extension() == ".jsonl" &&
        e.path().filename().string().find(infix) != std::string::npos)
      return e.path().string();
  return "";
}

long JsonInt(const std::string& line, const std::string& key) {
  std::string k = "\"" + key + "\":";
  auto p = line.find(k);
  if (p == std::string::npos) return 0;
  return std::atol(line.c_str() + p + k.size());
}

std::string JsonStr(const std::string& line, const std::string& key) {
  std::string k = "\"" + key + "\":\"";
  auto p = line.find(k);
  if (p == std::string::npos) return "";
  p += k.size();
  return line.substr(p, line.find('"', p) - p);
}

// Poll the rt journal until a record with {"e":"<rec>"} appears; returns that whole
// line, or "" on timeout.
std::string WaitRtRecord(const std::string& dir, const std::string& rec, int timeout_ms) {
  std::string needle = "\"e\":\"" + rec + "\"";
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    std::string path = FindJournal(dir, "-rt-");
    if (!path.empty()) {
      std::ifstream f(path);
      std::string line;
      while (std::getline(f, line))
        if (line.find(needle) != std::string::npos) return line;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return "";
}

bool RtHas(const std::string& dir, const std::string& rec) {
  std::string path = FindJournal(dir, "-rt-");
  return !path.empty() && Slurp(path).find("\"e\":\"" + rec + "\"") != std::string::npos;
}

struct Env {
  std::string root, sim_bin, bin_dir, hubd, trader, signald, tape;
  bool ok = false;
};

Env Probe() {
  Env e;
  e.root = RepoRoot();
  e.sim_bin = e.root + "/core/target/release/kairos-sim";
  e.bin_dir = e.root + "/core/target/release";
  e.hubd = e.root + "/exec/scenario/build/kairos_sim_hubd";
  e.trader = e.root + "/exec/scenario/build/kairos_scenario_trader";
  e.signald = e.root + "/exec/scenario/build/kairos_signald";
  e.tape = e.root + "/core/tests/fixtures/tapes/trend_day_2330.kqr";
  for (const std::string& p : {e.sim_bin, e.hubd, e.trader, e.signald, e.tape})
    if (!Exists(p)) {
      std::printf("test_roundtrip_fault_drills: SKIP (missing %s)\n", p.c_str());
      return e;
    }
  e.ok = true;
  return e;
}

// One drill's private, sim-namespaced world: every socket/dir/log lives under a
// per-drill temp base so parallel ctest runs never cross-wire (§7 sim discipline).
struct Ns {
  std::filesystem::path base;
  std::string aeron_dir, quote_sock, order_sock, signal_sock, spool, journal_dir;
  std::string scn_toml, signald_toml;
  std::string sim_log, signald_log, trader_log;
  std::string pidfile;
};

Ns MakeNs(const std::string& tag) {
  Ns n;
  n.base = std::filesystem::temp_directory_path() / ("kairos-rtfd-" + tag);
  std::filesystem::remove_all(n.base);
  std::filesystem::create_directories(n.base);
  n.aeron_dir = "/dev/shm/aeron-rtfd-" + tag;
  n.quote_sock = (n.base / "q.sock").string();
  n.order_sock = (n.base / "o.sock").string();
  n.signal_sock = (n.base / "s.sock").string();
  n.spool = (n.base / "signals.spool").string();
  n.journal_dir = (n.base / "journal").string();
  n.scn_toml = (n.base / "2330.toml").string();
  n.signald_toml = (n.base / "signald.toml").string();
  n.sim_log = (n.base / "sim.log").string();
  n.signald_log = (n.base / "signald.log").string();
  n.trader_log = (n.base / "trader.log").string();
  n.pidfile = n.aeron_dir + ".kairos-sim.pids";
  std::filesystem::create_directories(n.journal_dir);
  std::ofstream(n.spool).close();  // manual predicate anchors its offset at end-of-spool
  {
    std::ofstream f(n.signald_toml);
    f << "[[predicate]]\nkind=\"manual\"\nname=\"manual\"\nsymbols=[\"2330\"]\n";
  }
  return n;
}

// [roundtrip] scenario toml; `risk` and `rt` are drill-specific fragments.
void WriteScenario(const Ns& n, long budget, const std::string& risk, const std::string& rt) {
  std::ofstream f(n.scn_toml);
  f << "[scenario]\nname=\"2330\"\nsymbol=\"2330\"\nside=\"Buy\"\nbudget_twd=" << budget << "\n"
    << "board=\"RoundLot\"\n[pricing]\npolicy=\"cross\"\n"
    << risk << "[roundtrip]\nenabled=true\nsignal=\"manual\"\n"
    << rt << "[journal]\ndir=\"" << n.journal_dir << "\"\n";
}

// Point the sim at a fresh set of sockets/aeron dir (same journal_dir) so a restart
// binds cleanly instead of racing the previous run's stale socket files. Call after
// the previous sim is reaped so its now-free /dev/shm dir can be removed here.
void RotateSim(Ns& n, const std::string& suffix) {
  std::filesystem::remove_all(n.aeron_dir);
  std::filesystem::remove(n.pidfile);  // the reaped sim's now-free pidfile
  n.aeron_dir += suffix;
  n.quote_sock = (n.base / ("q" + suffix + ".sock")).string();
  n.order_sock = (n.base / ("o" + suffix + ".sock")).string();
  n.pidfile = n.aeron_dir + ".kairos-sim.pids";
}

pid_t StartSim(const Env& e, const Ns& n, const std::string& hubd_args, int speed) {
  pid_t sim = SpawnLogged(
      {e.sim_bin, "replay", e.tape, "--symbols", "2330", "--speed", std::to_string(speed),
       "--quote-sock", n.quote_sock, "--order-sock", n.order_sock, "--aeron-dir", n.aeron_dir,
       "--hubd", e.hubd, "--bin-dir", e.bin_dir},
      {{"KAIROS_SIM_HUBD_ARGS", hubd_args}}, n.sim_log, /*own_pgroup=*/true);
  return sim;
}

pid_t StartSignald(const Env& e, const Ns& n) {
  return SpawnLogged({e.signald, "--config", n.signald_toml, "--signal-sock", n.signal_sock,
                      "--quote-sock", n.quote_sock, "--spool", n.spool},
                     {}, n.signald_log, /*own_pgroup=*/true);
}

pid_t StartTrader(const Env& e, const Ns& n, const std::vector<std::string>& mode_args,
                  const std::string& wall_hhmm) {
  std::vector<std::string> argv = {e.trader, n.scn_toml};
  for (const auto& a : mode_args) argv.push_back(a);
  argv.insert(argv.end(), {"--yes", "--ignore-window", "--ignore-blacklist"});
  return SpawnLogged(argv,
                     {{"KAIROS_QUOTE_SOCK", n.quote_sock},
                      {"KAIROS_ORDER_SOCK", n.order_sock},
                      {"KAIROS_SIGNAL_SOCK", n.signal_sock},
                      {"KAIROS_RT_WALL_HHMM", wall_hhmm},
                      {"HOME", n.base.string()}},
                     n.trader_log, /*own_pgroup=*/false);
}

void InjectSignal(const Ns& n, const std::string& action) {
  std::ofstream f(n.spool, std::ios::app);
  f << "{\"signal\":\"manual\",\"symbol\":\"2330\",\"action\":\"" << action << "\"}\n";
}

// Reap the sim's recorded child process groups; return true if any leaked (orphan).
bool ReapSim(const Ns& n, pid_t sim) {
  std::vector<pid_t> pgids;
  if (Exists(n.pidfile)) {
    std::ifstream pf(n.pidfile);
    std::string ln;
    while (std::getline(pf, ln)) {
      long g = std::atol(ln.c_str());
      if (g > 0) pgids.push_back(static_cast<pid_t>(g));
    }
  }
  if (sim > 0) {
    ::kill(-sim, SIGTERM);
    ::waitpid(sim, nullptr, 0);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  bool orphan = false;
  for (pid_t g : pgids)
    if (::killpg(g, 0) == 0) {
      ::killpg(g, SIGKILL);
      orphan = true;
    }
  return orphan;
}

// Remove a sim's /dev/shm footprint: the aeron dir and its sibling pidfile.
void RemoveSimShm(const std::string& aeron_dir) {
  std::filesystem::remove_all(aeron_dir);
  std::filesystem::remove(aeron_dir + ".kairos-sim.pids");
}

void Cleanup(const Ns& n) {
  std::filesystem::remove_all(n.base);
  RemoveSimShm(n.aeron_dir);
}

const char* Ok(int rc) { return rc == 0 ? "OK" : "FAILED"; }

// -------------------------------------------------------------------------------

int Drill1(const Env& e) {
  // ENTER ack-drop storm: the enter leg (live, faulted hub) times out and halts with
  // a partial fill; the runner must transfer it into HOLD and keep the stop watchdog
  // live. Reuses test_sim_fault_drills' proven ack-drop-1.0 / seed-42 halt params.
  Ns n = MakeNs("d1-" + std::to_string(::getpid()));
  WriteScenario(
      n, 2000000,
      "[risk]\nack_timeout_ms=500\nmax_consecutive_order_failures=3\nstop_on_disconnect=true\n"
      "quote_stall_alert_ms=0\nquote_max_age_ms=60000\n",
      "stop_loss_pct=0.5\nmax_hold_min=5\nenter_window_min=10\narm_start=\"09:00\"\n"
      "arm_end=\"13:00\"\n");
  pid_t sim = StartSim(e, n, "--fault-seed 42 --fault-ack-drop-rate 1.0 --fault-partial 2", 8);
  pid_t sig = StartSignald(e, n);
  int rc = 0;
  if (!WaitForPath(n.quote_sock, 20000) || !WaitForPath(n.order_sock, 20000) ||
      !WaitForPath(n.signal_sock, 20000)) {
    std::printf("DRILL1: FAIL (pipeline sockets never appeared)\n");
    KillReap(sig, SIGKILL);
    ReapSim(n, sim);
    Cleanup(n);
    return 1;
  }
  pid_t trader = StartTrader(e, n, {"--live"}, "1000");
  bool armed = !WaitRtRecord(n.journal_dir, "armed", 15000).empty();
  std::this_thread::sleep_for(std::chrono::milliseconds(900));  // beat the subscribe race
  InjectSignal(n, "enter");
  if (WaitRtRecord(n.journal_dir, "trigger", 10000).empty())
    std::printf("DRILL1: FAIL (signal never triggered the arm — handoff broken)\n"), rc = 1;
  std::string enter_done = WaitRtRecord(n.journal_dir, "enter_done", 40000);
  long held = JsonInt(enter_done, "sh");
  std::string exit_trig = WaitRtRecord(n.journal_dir, "exit_trigger", 30000);
  int code = WaitExit(trader, 30000);

  std::string tlog = Slurp(n.trader_log);
  int timeouts = Count(tlog, "ack timeout");
  KillReap(sig, SIGTERM);
  bool orphan = ReapSim(n, sim);
  std::printf("DRILL1: armed=%d held=%ld exit_trig_reason=%s ack_timeouts=%d exit=%d orphan=%d\n",
              armed, held, JsonStr(exit_trig, "reason").c_str(), timeouts, code, orphan);
  if (held <= 0) {
    std::printf("DRILL1: FAIL (SAFETY: partial fill not transferred into HOLD)\n");
    rc = 1;
  }
  if (JsonStr(exit_trig, "reason") != "stop") {
    std::printf("DRILL1: FAIL (SAFETY: stop watchdog did not fire over the HOLD)\n");
    rc = 1;
  }
  if (timeouts < 3 || timeouts > 20) {
    std::printf("DRILL1: FAIL (SAFETY: order storm — submits not bounded: %d)\n", timeouts);
    rc = 1;
  }
  if (orphan) {
    std::printf("DRILL1: FAIL (orphan sim process group)\n");
    rc = 1;
  }
  Cleanup(n);
  std::printf("DRILL1: %s\n", Ok(rc));
  return rc;
}

int Drill2(const Env& e) {
  int rc = 0;
  // 2A: hold_with_stops — kill the daemon mid-HOLD, the stop still exits.
  {
    Ns n = MakeNs("d2a-" + std::to_string(::getpid()));
    WriteScenario(n, 2000000, "[risk]\nquote_stall_alert_ms=0\nquote_max_age_ms=60000\n",
                  "stop_loss_pct=0.5\nmax_hold_min=5\nenter_window_min=10\narm_start=\"09:00\"\n"
                  "arm_end=\"13:00\"\non_signal_loss=\"hold_with_stops\"\n");
    pid_t sim = StartSim(e, n, "", 8);
    pid_t sig = StartSignald(e, n);
    if (!WaitForPath(n.quote_sock, 20000) || !WaitForPath(n.signal_sock, 20000)) {
      std::printf("DRILL2A: FAIL (pipeline sockets never appeared)\n");
      KillReap(sig, SIGKILL);
      ReapSim(n, sim);
      Cleanup(n);
      return 1;
    }
    pid_t trader = StartTrader(e, n, {"--paper-instant"}, "1000");
    WaitRtRecord(n.journal_dir, "armed", 15000);
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    InjectSignal(n, "enter");
    bool held = !WaitRtRecord(n.journal_dir, "enter_done", 20000).empty();
    KillReap(sig, SIGKILL);  // daemon dies while the position is in HOLD
    std::string exit_trig = WaitRtRecord(n.journal_dir, "exit_trigger", 25000);
    int code = WaitExit(trader, 15000);
    bool orphan = ReapSim(n, sim);
    std::printf("DRILL2A: held=%d exit_reason=%s flat=%d exit=%d orphan=%d\n", held,
                JsonStr(exit_trig, "reason").c_str(), RtHas(n.journal_dir, "flat"), code, orphan);
    if (!held) std::printf("DRILL2A: FAIL (no HOLD to protect)\n"), rc = 1;
    if (JsonStr(exit_trig, "reason") != "stop") {
      std::printf("DRILL2A: FAIL (SAFETY: stop did not fire with the signal daemon dead)\n");
      rc = 1;
    }
    if (!RtHas(n.journal_dir, "flat") || code != 0)
      std::printf("DRILL2A: FAIL (SAFETY: stop exit did not reach a clean flat)\n"), rc = 1;
    if (orphan) std::printf("DRILL2A: FAIL (orphan)\n"), rc = 1;
    std::printf("DRILL2A: %s\n", Ok(rc == 0 ? 0 : 1));
    Cleanup(n);
  }
  // 2B: on_signal_loss=exit — killing the daemon runs a clean exit leg.
  {
    Ns n = MakeNs("d2b-" + std::to_string(::getpid()));
    WriteScenario(n, 2000000, "[risk]\nquote_stall_alert_ms=0\nquote_max_age_ms=60000\n",
                  "stop_loss_pct=15\nmax_hold_min=5\nenter_window_min=10\narm_start=\"09:00\"\n"
                  "arm_end=\"13:00\"\non_signal_loss=\"exit\"\n");
    pid_t sim = StartSim(e, n, "", 8);
    pid_t sig = StartSignald(e, n);
    if (!WaitForPath(n.quote_sock, 20000) || !WaitForPath(n.signal_sock, 20000)) {
      std::printf("DRILL2B: FAIL (pipeline sockets never appeared)\n");
      KillReap(sig, SIGKILL);
      ReapSim(n, sim);
      Cleanup(n);
      return 1;
    }
    pid_t trader = StartTrader(e, n, {"--paper-instant"}, "1000");
    WaitRtRecord(n.journal_dir, "armed", 15000);
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    InjectSignal(n, "enter");
    bool held = !WaitRtRecord(n.journal_dir, "enter_done", 20000).empty();
    KillReap(sig, SIGKILL);
    bool exited = !WaitRtRecord(n.journal_dir, "exit_trigger", 25000).empty();
    int code = WaitExit(trader, 15000);
    bool orphan = ReapSim(n, sim);
    std::printf("DRILL2B: held=%d exit_leg=%d flat=%d exit=%d orphan=%d\n", held, exited,
                RtHas(n.journal_dir, "flat"), code, orphan);
    if (!held) std::printf("DRILL2B: FAIL (no HOLD to protect)\n"), rc = 1;
    if (!exited || !RtHas(n.journal_dir, "flat") || code != 0) {
      std::printf("DRILL2B: FAIL (SAFETY: signal-loss exit policy did not run a clean exit)\n");
      rc = 1;
    }
    if (orphan) std::printf("DRILL2B: FAIL (orphan)\n"), rc = 1;
    std::printf("DRILL2B: %s\n", Ok(rc == 0 ? 0 : 1));
    Cleanup(n);
  }
  return rc;
}

int Drill3(const Env& e) {
  // EXIT reject storm, staged through PR5 recovery so the faults never touch the enter
  // leg: (A) a clean paper enter, then (B) a restarted live trader on the same journal
  // resumes HOLD and hits a reject storm on the exit => FATAL naming the remaining
  // shares, non-zero exit, never a silent flat.
  Ns n = MakeNs("d3-" + std::to_string(::getpid()));
  WriteScenario(n, 2000000,
                "[risk]\nack_timeout_ms=500\nmax_consecutive_order_failures=3\n"
                "quote_stall_alert_ms=0\nquote_max_age_ms=60000\nstop_on_disconnect=true\n",
                "stop_loss_pct=15\nmax_hold_min=5\nenter_window_min=10\narm_start=\"09:00\"\n"
                "arm_end=\"13:00\"\non_signal_loss=\"hold_with_stops\"\n");
  int rc = 0;
  // Phase A: clean paper enter, then a graceful stop leaves the position in the journal.
  pid_t sim = StartSim(e, n, "", 8);
  pid_t sig = StartSignald(e, n);
  if (!WaitForPath(n.quote_sock, 20000) || !WaitForPath(n.order_sock, 20000) ||
      !WaitForPath(n.signal_sock, 20000)) {
    std::printf("DRILL3: FAIL (pipeline sockets never appeared)\n");
    KillReap(sig, SIGKILL);
    ReapSim(n, sim);
    Cleanup(n);
    return 1;
  }
  pid_t t1 = StartTrader(e, n, {"--paper-instant"}, "1000");
  WaitRtRecord(n.journal_dir, "armed", 15000);
  std::this_thread::sleep_for(std::chrono::milliseconds(900));
  InjectSignal(n, "enter");
  long entered = JsonInt(WaitRtRecord(n.journal_dir, "enter_done", 20000), "sh");
  KillReap(t1, SIGTERM);  // graceful stop; net>0 stays in the Buy journal for recovery

  // Phase B: restart live into a total reject storm; the exit leg must halt loud.
  std::filesystem::remove(FindJournal(n.journal_dir, "-rt-"));  // clear the phase-A rt terminal
  ReapSim(n, sim);
  RotateSim(n, "b");  // fresh sockets: the restart hub inherits KAIROS_SIM_HUBD_ARGS
  pid_t sim2 = StartSim(e, n, "--fault-seed 7 --fault-reject-rate 1.0", 8);
  if (!WaitForPath(n.quote_sock, 20000) || !WaitForPath(n.order_sock, 20000)) {
    std::printf("DRILL3: FAIL (restart hub sockets never appeared)\n");
    KillReap(sig, SIGKILL);
    ReapSim(n, sim2);
    Cleanup(n);
    return 1;
  }
  pid_t t2 = StartTrader(e, n, {"--live"}, "1000");
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));  // recovery + subscribe settle
  InjectSignal(n, "exit");
  std::string failed = WaitRtRecord(n.journal_dir, "failed", 30000);
  long remaining = JsonInt(failed, "sh_remaining");
  int code = WaitExit(t2, 20000);
  std::string tlog = Slurp(n.trader_log);
  bool fatal = tlog.find("FATAL") != std::string::npos;
  KillReap(sig, SIGTERM);
  bool orphan = ReapSim(n, sim2);
  std::printf("DRILL3: entered=%ld failed_remaining=%ld fatal=%d flat=%d exit=%d orphan=%d\n",
              entered, remaining, fatal, RtHas(n.journal_dir, "flat"), code, orphan);
  if (entered <= 0) std::printf("DRILL3: FAIL (phase-A enter did not fill)\n"), rc = 1;
  if (RtHas(n.journal_dir, "flat")) {
    std::printf("DRILL3: FAIL (SAFETY: silent flat — exit reject storm hid a live position)\n");
    rc = 1;
  }
  if (remaining <= 0 || !fatal || code == 0) {
    std::printf("DRILL3: FAIL (SAFETY: exit halt not loud with remaining shares named)\n");
    rc = 1;
  }
  if (orphan) std::printf("DRILL3: FAIL (orphan)\n"), rc = 1;
  Cleanup(n);
  std::printf("DRILL3: %s\n", Ok(rc));
  return rc;
}

int Drill4(const Env& e) {
  // HOLD quote-socket break: kill the whole sim feed mid-HOLD. The staleness hard stop
  // must force an exit attempt (exit_trigger) — the position is never left silently
  // unwatched — and the run ends fail-closed (flat, or failed with shares named).
  Ns n = MakeNs("d4-" + std::to_string(::getpid()));
  WriteScenario(n, 2000000, "[risk]\nquote_stall_alert_ms=2000\nquote_max_age_ms=60000\n",
                "stop_loss_pct=15\nmax_hold_min=5\nenter_window_min=10\narm_start=\"09:00\"\n"
                "arm_end=\"13:00\"\non_signal_loss=\"hold_with_stops\"\n");
  int rc = 0;
  pid_t sim = StartSim(e, n, "", 8);
  pid_t sig = StartSignald(e, n);
  if (!WaitForPath(n.quote_sock, 20000) || !WaitForPath(n.signal_sock, 20000)) {
    std::printf("DRILL4: FAIL (pipeline sockets never appeared)\n");
    KillReap(sig, SIGKILL);
    ReapSim(n, sim);
    Cleanup(n);
    return 1;
  }
  pid_t trader = StartTrader(e, n, {"--paper-instant"}, "1000");
  WaitRtRecord(n.journal_dir, "armed", 15000);
  std::this_thread::sleep_for(std::chrono::milliseconds(900));
  InjectSignal(n, "enter");
  bool held = !WaitRtRecord(n.journal_dir, "enter_done", 20000).empty();
  bool sim_orphan = ReapSim(n, sim);  // the quote feed dies mid-HOLD
  // The staleness hard stop must force an exit; the sell then cannot fill against a
  // dead feed, so a graceful stop drives the fail-closed terminal (never a bare HOLD).
  std::string exit_trig = WaitRtRecord(n.journal_dir, "exit_trigger", 15000);
  KillReap(sig, SIGTERM);
  ::kill(trader, SIGTERM);
  int code = WaitExit(trader, 12000);
  bool terminal = RtHas(n.journal_dir, "flat") || RtHas(n.journal_dir, "failed");
  std::printf("DRILL4: held=%d forced_exit_reason=%s terminal=%d exit=%d\n", held,
              JsonStr(exit_trig, "reason").c_str(), terminal, code);
  if (!held) std::printf("DRILL4: FAIL (no HOLD to protect)\n"), rc = 1;
  if (exit_trig.empty()) {
    std::printf("DRILL4: FAIL (SAFETY: staleness did not force an exit — position unwatched)\n");
    rc = 1;
  }
  if (!terminal || code == 0) {
    std::printf("DRILL4: FAIL (SAFETY: no fail-closed terminal — position left in limbo)\n");
    rc = 1;
  }
  if (sim_orphan) std::printf("DRILL4: FAIL (orphan)\n"), rc = 1;
  Cleanup(n);
  std::printf("DRILL4: %s\n", Ok(rc));
  return rc;
}

int Drill5a(const Env& e) {
  // EXIT hub disconnect, staged through PR5 recovery so the drop lands squarely on the
  // exit leg: (A) a clean paper enter, then (B) a restarted live trader whose exit
  // submit trips the hub disconnect => stop_on_disconnect stops the leg fail-closed with
  // the position still remaining (never a silent flat).
  Ns n = MakeNs("d5a-" + std::to_string(::getpid()));
  WriteScenario(n, 2000000,
                "[risk]\nack_timeout_ms=3000\nmax_consecutive_order_failures=0\n"
                "quote_stall_alert_ms=0\nquote_max_age_ms=60000\nstop_on_disconnect=true\n",
                "stop_loss_pct=15\nmax_hold_min=5\nenter_window_min=10\narm_start=\"09:00\"\n"
                "arm_end=\"13:00\"\non_signal_loss=\"hold_with_stops\"\n");
  int rc = 0;
  pid_t sim = StartSim(e, n, "", 8);
  pid_t sig = StartSignald(e, n);
  if (!WaitForPath(n.quote_sock, 20000) || !WaitForPath(n.order_sock, 20000) ||
      !WaitForPath(n.signal_sock, 20000)) {
    std::printf("DRILL5A: FAIL (pipeline sockets never appeared)\n");
    KillReap(sig, SIGKILL);
    ReapSim(n, sim);
    Cleanup(n);
    return 1;
  }
  pid_t t1 = StartTrader(e, n, {"--paper-instant"}, "1000");
  WaitRtRecord(n.journal_dir, "armed", 15000);
  std::this_thread::sleep_for(std::chrono::milliseconds(900));
  InjectSignal(n, "enter");
  long entered = JsonInt(WaitRtRecord(n.journal_dir, "enter_done", 20000), "sh");
  KillReap(t1, SIGTERM);

  std::filesystem::remove(FindJournal(n.journal_dir, "-rt-"));
  ReapSim(n, sim);
  RotateSim(n, "b");
  pid_t sim2 = StartSim(e, n, "--fault-disconnect-after-n 1", 8);  // drop the exit's 1st submit
  if (!WaitForPath(n.quote_sock, 20000) || !WaitForPath(n.order_sock, 20000)) {
    std::printf("DRILL5A: FAIL (restart hub sockets never appeared)\n");
    KillReap(sig, SIGKILL);
    ReapSim(n, sim2);
    Cleanup(n);
    return 1;
  }
  pid_t t2 = StartTrader(e, n, {"--live"}, "1000");
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  InjectSignal(n, "exit");
  std::string failed = WaitRtRecord(n.journal_dir, "failed", 25000);
  long remaining = JsonInt(failed, "sh_remaining");
  int code = WaitExit(t2, 20000);
  std::string tlog = Slurp(n.trader_log);
  bool stopped = tlog.find("order backend disconnected; stopping") != std::string::npos;
  KillReap(sig, SIGTERM);
  bool orphan = ReapSim(n, sim2);
  std::printf(
      "DRILL5A: entered=%ld disconnect_stop=%d failed_remaining=%ld flat=%d exit=%d "
      "orphan=%d\n",
      entered, stopped, remaining, RtHas(n.journal_dir, "flat"), code, orphan);
  if (entered <= 0) std::printf("DRILL5A: FAIL (phase-A enter did not fill)\n"), rc = 1;
  if (!stopped) {
    std::printf("DRILL5A: FAIL (SAFETY: hub disconnect did not stop the leg fail-closed)\n");
    rc = 1;
  }
  if (remaining <= 0 || RtHas(n.journal_dir, "flat") || code == 0) {
    std::printf("DRILL5A: FAIL (SAFETY: disconnect hid a remaining position / silent flat)\n");
    rc = 1;
  }
  if (orphan) std::printf("DRILL5A: FAIL (orphan)\n"), rc = 1;
  Cleanup(n);
  std::printf("DRILL5A: %s\n", Ok(rc));
  return rc;
}

int Drill5b(const Env& e) {
  // SIGKILL-in-HOLD restart (the CI kill -9): a clean enter reaches HOLD, kill -9 the
  // trader, restart it on the same journal => PR5 recovery resumes protection, and a
  // manual exit then flats. What this proves is the ungraceful crash + journal recovery
  // path, so both legs run paper-instant for round, deterministic fills.
  Ns n = MakeNs("d5b-" + std::to_string(::getpid()));
  WriteScenario(n, 2000000, "[risk]\nquote_stall_alert_ms=0\nquote_max_age_ms=60000\n",
                "stop_loss_pct=15\nmax_hold_min=5\nenter_window_min=10\narm_start=\"09:00\"\n"
                "arm_end=\"13:00\"\non_signal_loss=\"hold_with_stops\"\n");
  int rc = 0;
  pid_t sim = StartSim(e, n, "", 8);
  pid_t sig = StartSignald(e, n);
  if (!WaitForPath(n.quote_sock, 20000) || !WaitForPath(n.order_sock, 20000) ||
      !WaitForPath(n.signal_sock, 20000)) {
    std::printf("DRILL5B: FAIL (pipeline sockets never appeared)\n");
    KillReap(sig, SIGKILL);
    ReapSim(n, sim);
    Cleanup(n);
    return 1;
  }
  pid_t t1 = StartTrader(e, n, {"--paper-instant"}, "1000");
  WaitRtRecord(n.journal_dir, "armed", 15000);
  std::this_thread::sleep_for(std::chrono::milliseconds(900));
  InjectSignal(n, "enter");
  long entered = JsonInt(WaitRtRecord(n.journal_dir, "enter_done", 25000), "sh");
  KillReap(t1, SIGKILL);  // ungraceful crash mid-HOLD; reaped before the restart races

  pid_t t2 = StartTrader(e, n, {"--paper-instant"}, "1000");
  // recover-hold is sink-only; the restart re-arms nothing (net>0 resumes HOLD), so the
  // observable proof of resumed protection is that a manual exit now flats the position.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  InjectSignal(n, "exit");
  bool exited = !WaitRtRecord(n.journal_dir, "exit_trigger", 25000).empty();
  bool flat = !WaitRtRecord(n.journal_dir, "flat", 20000).empty();
  int code = WaitExit(t2, 15000);
  KillReap(sig, SIGTERM);
  bool orphan = ReapSim(n, sim);
  std::printf("DRILL5B: entered=%ld exit_leg=%d flat=%d exit=%d orphan=%d\n", entered, exited, flat,
              code, orphan);
  if (entered <= 0) std::printf("DRILL5B: FAIL (enter did not fill before the kill)\n"), rc = 1;
  if (!exited || !flat || code != 0) {
    std::printf("DRILL5B: FAIL (SAFETY: crash restart did not resume protection and exit)\n");
    rc = 1;
  }
  if (orphan) std::printf("DRILL5B: FAIL (orphan)\n"), rc = 1;
  Cleanup(n);
  std::printf("DRILL5B: %s\n", Ok(rc));
  return rc;
}

int Drill6(const Env& e) {
  // Degenerate late trigger: a manual enter injected after the 13:25 window arithmetic
  // is dropped — armed, but no trigger and no leg — closing the PR3 handoff loop.
  Ns n = MakeNs("d6-" + std::to_string(::getpid()));
  WriteScenario(n, 2000000, "[risk]\nquote_stall_alert_ms=0\nquote_max_age_ms=60000\n",
                "stop_loss_pct=5\nmax_hold_min=5\nenter_window_min=10\narm_start=\"09:00\"\n"
                "arm_end=\"13:00\"\n");
  int rc = 0;
  pid_t sim = StartSim(e, n, "", 8);
  pid_t sig = StartSignald(e, n);
  if (!WaitForPath(n.quote_sock, 20000) || !WaitForPath(n.signal_sock, 20000)) {
    std::printf("DRILL6: FAIL (pipeline sockets never appeared)\n");
    KillReap(sig, SIGKILL);
    ReapSim(n, sim);
    Cleanup(n);
    return 1;
  }
  pid_t trader = StartTrader(e, n, {"--paper-instant"}, "1326");  // now >= 13:25
  bool armed = !WaitRtRecord(n.journal_dir, "armed", 15000).empty();
  std::this_thread::sleep_for(std::chrono::milliseconds(900));
  InjectSignal(n, "enter");
  std::this_thread::sleep_for(
      std::chrono::milliseconds(3000));  // give any (wrong) leg time to start
  bool triggered = RtHas(n.journal_dir, "trigger");
  bool entered = RtHas(n.journal_dir, "enter_done");
  bool buy_fills = !FindJournal(n.journal_dir, "-Buy-").empty();
  KillReap(sig, SIGTERM);
  ::kill(trader, SIGTERM);
  int code = WaitExit(trader, 8000);
  bool orphan = ReapSim(n, sim);
  std::printf("DRILL6: armed=%d triggered=%d entered=%d buy_fills=%d exit=%d orphan=%d\n", armed,
              triggered, entered, buy_fills, code, orphan);
  if (!armed) std::printf("DRILL6: FAIL (never armed)\n"), rc = 1;
  if (triggered || entered || buy_fills) {
    std::printf("DRILL6: FAIL (SAFETY: a leg started for a past-13:25 trigger)\n");
    rc = 1;
  }
  if (orphan) std::printf("DRILL6: FAIL (orphan)\n"), rc = 1;
  Cleanup(n);
  std::printf("DRILL6: %s\n", Ok(rc));
  return rc;
}

}  // namespace

int main() {
  if (std::getenv("KAIROS_FAULT_DRILLS") == nullptr) {
    std::printf("test_roundtrip_fault_drills: SKIP (set KAIROS_FAULT_DRILLS=1 to run)\n");
    return 0;
  }
  Env e = Probe();
  if (!e.ok) return 0;

  int rc = 0;
  rc |= Drill1(e);
  rc |= Drill2(e);
  rc |= Drill3(e);
  rc |= Drill4(e);
  rc |= Drill5a(e);
  rc |= Drill5b(e);
  rc |= Drill6(e);
  std::printf("test_roundtrip_fault_drills: %s\n", Ok(rc));
  return rc;
}
