// Regression drills: rehearse the sim hub's fault paths against the REAL built
// binaries (kairos-sim pipeline + a fault-configured kairos_sim_hubd + a real
// kairos_scenario_trader), turning past incidents into repeatable off-hours drills.
//
//   DRILL 1  the 7/6 ack-drop storm: ack_drop_rate=1.0 => the trader submits, times
//            out, cancels the timed-out id, and HALTS after exactly
//            max_consecutive_order_failures (PR #112). Assert bounded submits, the
//            cancels, non-zero exit, and no orphan process group.
//   DRILL 2  reject storm: reject_rate=1.0 => halt after N rejects, no cancels.
//   DRILL 3  late fill after cancel (PR #61): partial_fill + ack_delay so a fill
//            lands after the trader cancelled/re-pegged; assert the journal shows a
//            fill for an id already cancelled and the run stays consistent.
//   DRILL 4  disconnect: the hub drops the client mid-run => stop_on_disconnect
//            teardown, clean exit, no orphan.
//
// Faults reach the hubd through the untouched kairos-sim pipeline via the
// KAIROS_SIM_HUBD_ARGS env var (kairos-sim's hubd child inherits it). Opt-in: needs
// the sim + C++ bins, so it self-skips (exit 0) unless KAIROS_FAULT_DRILLS=1.

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
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

// Reap `pid`, returning its exit code, or -1 on timeout (then SIGKILL it).
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

std::string JournalFile(const std::string& dir) {
  if (!Exists(dir)) return "";
  for (const auto& e : std::filesystem::directory_iterator(dir))
    if (e.path().extension() == ".jsonl") return e.path().string();
  return "";
}

std::string JsonStr(const std::string& line, const std::string& key) {
  std::string k = "\"" + key + "\":\"";
  auto p = line.find(k);
  if (p == std::string::npos) return "";
  p += k.size();
  return line.substr(p, line.find('"', p) - p);
}

long JsonInt(const std::string& line, const std::string& key) {
  std::string k = "\"" + key + "\":";
  auto p = line.find(k);
  if (p == std::string::npos) return 0;
  return std::atol(line.c_str() + p + k.size());
}

struct Env {
  std::string root, sim_bin, bin_dir, hubd, trader, tape;
  bool ok = false;
};

Env Probe() {
  Env e;
  e.root = RepoRoot();
  e.sim_bin = e.root + "/core/target/release/kairos-sim";
  e.bin_dir = e.root + "/core/target/release";
  e.hubd = e.root + "/exec/scenario/build/kairos_sim_hubd";
  e.trader = e.root + "/exec/scenario/build/kairos_scenario_trader";
  e.tape = e.root + "/core/tests/fixtures/tapes/trend_day_2330.kqr";
  for (const std::string& p : {e.sim_bin, e.hubd, e.trader, e.tape})
    if (!Exists(p)) {
      std::printf("test_sim_fault_drills: SKIP (missing %s)\n", p.c_str());
      return e;
    }
  e.ok = true;
  return e;
}

// One drill run: brings up the sim with the fault args, spawns the trader directly
// (so its exit code is observed), waits for it to finish (or SIGTERMs it after
// run_ms if it does not self-halt), then tears the sim down and checks for orphans.
struct RunResult {
  int exit_code = -999;
  std::string trader_log;
  std::string journal;
  bool orphan = false;
};

RunResult RunDrill(const Env& e, const std::string& tag, const std::string& hubd_args,
                   const std::string& risk_toml, long run_ms, bool expect_self_exit) {
  RunResult rr;
  std::filesystem::path base = std::filesystem::temp_directory_path() / ("kairos-fd-" + tag);
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base);
  const std::string aeron_dir = "/dev/shm/aeron-fd-" + tag;
  const std::string quote_sock = (base / "q.sock").string();
  const std::string order_sock = (base / "o.sock").string();
  const std::string journal_dir = (base / "journal").string();
  const std::string toml = (base / "2330.toml").string();
  const std::string sim_log = (base / "sim.log").string();
  const std::string trader_log = (base / "trader.log").string();
  std::filesystem::create_directories(journal_dir);
  {
    std::ofstream f(toml);
    f << "[scenario]\nname=\"2330\"\nsymbol=\"2330\"\nside=\"Buy\"\nbudget_twd=2000000\n"
      << "board=\"RoundLot\"\n[pricing]\npolicy=\"cross\"\n"
      << risk_toml << "[journal]\ndir=\"" << journal_dir << "\"\n";
  }
  const std::string pidfile = aeron_dir + ".kairos-sim.pids";

  pid_t sim = SpawnLogged({e.sim_bin, "replay", e.tape, "--symbols", "2330", "--speed", "8",
                           "--quote-sock", quote_sock, "--order-sock", order_sock, "--aeron-dir",
                           aeron_dir, "--hubd", e.hubd, "--bin-dir", e.bin_dir},
                          {{"KAIROS_SIM_HUBD_ARGS", hubd_args}}, sim_log, /*own_pgroup=*/true);
  std::printf("%s: kairos-sim pid=%d hubd_args=[%s]\n", tag.c_str(), sim, hubd_args.c_str());
  if (!WaitForPath(quote_sock, 20000) || !WaitForPath(order_sock, 20000)) {
    std::printf("%s: FAIL (sim sockets never appeared)\n", tag.c_str());
    ::kill(-sim, SIGKILL);
    ::waitpid(sim, nullptr, 0);
    return rr;
  }

  pid_t trader =
      SpawnLogged({e.trader, toml, "--live", "--yes", "--ignore-window", "--ignore-blacklist"},
                  {{"KAIROS_QUOTE_SOCK", quote_sock},
                   {"KAIROS_ORDER_SOCK", order_sock},
                   {"HOME", base.string()}},
                  trader_log, /*own_pgroup=*/false);
  std::printf("%s: trader pid=%d\n", tag.c_str(), trader);

  if (expect_self_exit) {
    rr.exit_code = WaitExit(trader, run_ms);
  } else {
    std::this_thread::sleep_for(std::chrono::milliseconds(run_ms));
    ::kill(trader, SIGTERM);
    rr.exit_code = WaitExit(trader, 8000);
  }

  std::vector<pid_t> sim_pgids;
  if (Exists(pidfile)) {
    std::ifstream pf(pidfile);
    std::string ln;
    while (std::getline(pf, ln)) {
      long g = std::atol(ln.c_str());
      if (g > 0) sim_pgids.push_back(static_cast<pid_t>(g));
    }
  }
  ::kill(-sim, SIGTERM);
  ::waitpid(sim, nullptr, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  for (pid_t g : sim_pgids)
    if (::killpg(g, 0) == 0) {
      ::killpg(g, SIGKILL);
      rr.orphan = true;
    }

  rr.trader_log = Slurp(trader_log);
  rr.journal = Slurp(JournalFile(journal_dir));
  std::printf("%s: trader exit=%d, journal %zu bytes\n", tag.c_str(), rr.exit_code,
              rr.journal.size());
  std::filesystem::remove_all(base);
  std::filesystem::remove_all(aeron_dir);
  return rr;
}

int Drill1(const Env& e) {
  RunResult r = RunDrill(
      e, "drill1-ackdrop-" + std::to_string(::getpid()),
      "--fault-seed 42 --fault-ack-drop-rate 1.0",
      "[risk]\nack_timeout_ms=500\nmax_consecutive_order_failures=3\nstop_on_disconnect=true\n",
      45000, /*expect_self_exit=*/true);
  int rc = 0;
  int timeouts = Count(r.trader_log, "ack timeout");
  int cancels = Count(r.journal, "\"type\":\"cancel\"");
  bool halted = r.trader_log.find("FATAL") != std::string::npos &&
                r.trader_log.find("consecutive order failures") != std::string::npos;
  std::printf("DRILL1: exit=%d ack_timeouts=%d journal_cancels=%d halted=%d orphan=%d\n",
              r.exit_code, timeouts, cancels, halted, r.orphan);
  if (r.exit_code != 17) {
    std::printf("DRILL1: FAIL (expected halt exit 17, got %d)\n", r.exit_code);
    rc = 1;
  }
  if (!halted) {
    std::printf("DRILL1: FAIL (no #112 FATAL halt line)\n");
    rc = 1;
  }
  if (timeouts < 3 || timeouts > 8) {  // bounded: no order storm
    std::printf("DRILL1: FAIL (unbounded/insufficient submits: %d ack timeouts)\n", timeouts);
    rc = 1;
  }
  if (cancels < 1) {
    std::printf("DRILL1: FAIL (no cancel of a timed-out id)\n");
    rc = 1;
  }
  if (r.orphan) {
    std::printf("DRILL1: FAIL (orphan sim process group)\n");
    rc = 1;
  }
  std::printf("DRILL1: %s\n", rc == 0 ? "OK" : "FAILED");
  return rc;
}

int Drill2(const Env& e) {
  RunResult r = RunDrill(e, "drill2-reject-" + std::to_string(::getpid()),
                         "--fault-seed 7 --fault-reject-rate 1.0",
                         "[risk]\nack_timeout_ms=500\nmax_consecutive_order_failures=3\n", 45000,
                         /*expect_self_exit=*/true);
  int rc = 0;
  int rejects = Count(r.trader_log, "rejected");
  int cancels = Count(r.journal, "\"type\":\"cancel\"");
  bool halted = r.trader_log.find("FATAL") != std::string::npos;
  std::printf("DRILL2: exit=%d rejects=%d journal_cancels=%d halted=%d orphan=%d\n", r.exit_code,
              rejects, cancels, halted, r.orphan);
  if (r.exit_code != 17 || !halted) {
    std::printf("DRILL2: FAIL (expected reject-storm halt exit 17)\n");
    rc = 1;
  }
  if (rejects < 3) {
    std::printf("DRILL2: FAIL (fewer than N rejects observed)\n");
    rc = 1;
  }
  if (r.orphan) rc = 1;
  std::printf("DRILL2: %s\n", rc == 0 ? "OK" : "FAILED");
  return rc;
}

int Drill3(const Env& e) {
  // Late fill after cancel: acks are delayed well past the ack timeout, so every
  // order times out and is cancelled, yet the engine fills it (split into partials)
  // and routes the fills by id. Halt is disabled so the run keeps going and late
  // fills accumulate; accounting must stay consistent (PR #61).
  RunResult r = RunDrill(
      e, "drill3-latefill-" + std::to_string(::getpid()),
      "--fault-seed 9 --fault-ack-delay-ms 1500 --fault-partial 3",
      "[risk]\nack_timeout_ms=500\nmax_consecutive_order_failures=0\nstop_on_disconnect=false\n",
      20000, /*expect_self_exit=*/false);
  int rc = 0;

  // Scan the journal in event order. ack_delay pushes each ok ack past the 500ms
  // ack timeout, so every order times out and is cancelled while it is still
  // partially filling at the engine, and its ack lands AFTER the cancel (the "live
  // at broker, ack late" case). The regression is that fills on an id the trader has
  // already timed out and cancelled are still counted exactly once.
  long fill_shares = 0;
  bool negative = false;
  int fill_and_cancel_ids = 0;  // orders both partially filled and cancelled
  int ack_after_cancel = 0;     // a delayed ack that arrived after its own cancel
  {
    std::map<std::string, bool> filled, cancelled;
    std::stringstream ss(r.journal);
    std::string line;
    while (std::getline(ss, line)) {
      if (line.empty()) continue;
      std::string type = JsonStr(line, "type");
      std::string id = JsonStr(line, "id");
      if (type == "fill") {
        long sh = JsonInt(line, "shares");
        if (sh < 0) negative = true;
        fill_shares += sh;
        filled[id] = true;
      } else if (type == "cancel") {
        cancelled[id] = true;
      } else if (type == "ack") {
        if (cancelled[id]) ++ack_after_cancel;
      }
    }
    for (const auto& [id, f] : filled)
      if (f && cancelled[id]) ++fill_and_cancel_ids;
  }
  // Cross-check the trader's own tally against the journal (no double count / loss).
  long reported = -1;
  auto p = r.trader_log.find("kairos-exec: end - filled ");
  if (p != std::string::npos) reported = std::atol(r.trader_log.c_str() + p + 26);

  std::printf(
      "DRILL3: exit=%d fill_shares=%ld reported=%ld fill+cancel_ids=%d ack_after_cancel=%d "
      "negative=%d\n",
      r.exit_code, fill_shares, reported, fill_and_cancel_ids, ack_after_cancel, negative);
  if (r.exit_code != 0) {
    std::printf("DRILL3: FAIL (run did not exit cleanly)\n");
    rc = 1;
  }
  if (negative) {
    std::printf("DRILL3: FAIL (negative fill shares — corrupt accounting)\n");
    rc = 1;
  }
  if (fill_shares <= 0) {
    std::printf("DRILL3: FAIL (no fills — cannot exercise late-fill accounting)\n");
    rc = 1;
  }
  if (reported != fill_shares) {
    std::printf("DRILL3: FAIL (trader tally %ld != journal fills %ld)\n", reported, fill_shares);
    rc = 1;
  }
  if (fill_and_cancel_ids < 1) {
    std::printf("DRILL3: FAIL (no order both filled and cancelled — race not exercised)\n");
    rc = 1;
  }
  if (ack_after_cancel < 1) {
    std::printf("DRILL3: FAIL (no delayed ack landed after its cancel)\n");
    rc = 1;
  }
  if (r.orphan) rc = 1;
  std::printf("DRILL3: %s\n", rc == 0 ? "OK" : "FAILED");
  return rc;
}

int Drill4(const Env& e) {
  RunResult r =
      RunDrill(e, "drill4-disc-" + std::to_string(::getpid()), "--fault-disconnect-after-n 2",
               "[risk]\nack_timeout_ms=3000\nmax_consecutive_order_failures=0\n"
               "stop_on_disconnect=true\n",
               30000, /*expect_self_exit=*/true);
  int rc = 0;
  bool stopped = r.trader_log.find("order backend disconnected; stopping") != std::string::npos;
  std::printf("DRILL4: exit=%d stop_on_disconnect=%d orphan=%d\n", r.exit_code, stopped, r.orphan);
  if (r.exit_code != 0) {
    std::printf("DRILL4: FAIL (unclean teardown, exit %d)\n", r.exit_code);
    rc = 1;
  }
  if (!stopped) {
    std::printf("DRILL4: FAIL (trader did not stop on disconnect)\n");
    rc = 1;
  }
  if (r.orphan) rc = 1;
  std::printf("DRILL4: %s\n", rc == 0 ? "OK" : "FAILED");
  return rc;
}

}  // namespace

int main() {
  if (std::getenv("KAIROS_FAULT_DRILLS") == nullptr) {
    std::printf("test_sim_fault_drills: SKIP (set KAIROS_FAULT_DRILLS=1 to run)\n");
    return 0;
  }
  Env e = Probe();
  if (!e.ok) return 0;

  int rc = 0;
  rc |= Drill1(e);
  rc |= Drill2(e);
  rc |= Drill3(e);
  rc |= Drill4(e);
  std::printf("test_sim_fault_drills: %s\n", rc == 0 ? "OK" : "FAILED");
  return rc;
}
