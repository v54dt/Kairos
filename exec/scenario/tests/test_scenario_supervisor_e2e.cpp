// End-to-end proof: the supervisor daemon owns a real scenario trader against an
// isolated kairos-sim. Brings up the sim on a fixture tape, points the daemon at
// the sim sockets, starts scenario 2330 in test mode, polls list until the state
// advances with cum_fills>0, stops it, and asserts no trader survives and the sim
// children are reaped. Never touches the live KAIROS_* sockets or the real hub.
//
// Opt-in: needs the sim + C++ bins, so it self-skips (exit 0) unless
// KAIROS_SUPERVISOR_E2E=1 is set with the binary paths resolvable.

#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
#include <string>
#include <thread>
#include <vector>

namespace {

std::string RepoRoot() {
  // This file lives at <root>/exec/scenario/tests/; walk up three levels.
  std::filesystem::path here = __FILE__;
  return here.parent_path().parent_path().parent_path().parent_path().string();
}

bool Exists(const std::string& p) { return std::filesystem::exists(p); }

pid_t SpawnDetached(const std::vector<std::string>& argv,
                    const std::vector<std::pair<std::string, std::string>>& env, bool own_pgroup) {
  pid_t pid = ::fork();
  if (pid != 0) return pid;
  if (own_pgroup) ::setpgid(0, 0);  // sim: its own group so we can prove it dies
  for (const auto& [k, v] : env) ::setenv(k.c_str(), v.c_str(), 1);
  std::vector<char*> cargv;
  for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
  cargv.push_back(nullptr);
  ::execv(cargv[0], cargv.data());
  ::_exit(127);
}

int ConnectCtl(const std::string& path) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  timeval tv{3, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

// Send one request line and read the single response line.
std::string Request(const std::string& sock, const std::string& line) {
  int fd = ConnectCtl(sock);
  if (fd < 0) return "";
  std::string msg = line + "\n";
  if (::write(fd, msg.data(), msg.size()) < 0) {
    ::close(fd);
    return "";
  }
  std::string resp;
  char c;
  while (resp.size() < 1 << 20) {
    ssize_t n = ::recv(fd, &c, 1, 0);
    if (n <= 0 || c == '\n') break;
    resp.push_back(c);
  }
  ::close(fd);
  return resp;
}

bool WaitForPath(const std::string& p, int timeout_ms) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (Exists(p)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return Exists(p);
}

long FieldLong(const std::string& json, const std::string& key) {
  std::string k = "\"" + key + "\":";
  std::size_t at = json.find(k);
  if (at == std::string::npos) return -1;
  at += k.size();
  return std::atol(json.c_str() + at);
}

std::string FieldStr(const std::string& json, const std::string& key) {
  std::string k = "\"" + key + "\":\"";
  std::size_t at = json.find(k);
  if (at == std::string::npos) return "";
  at += k.size();
  std::size_t end = json.find('"', at);
  return json.substr(at, end - at);
}

bool WaitPidGone(pid_t pid, int timeout_ms) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (::kill(pid, 0) == -1 && errno == ESRCH) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return ::kill(pid, 0) == -1 && errno == ESRCH;
}

}  // namespace

int main() {
  if (std::getenv("KAIROS_SUPERVISOR_E2E") == nullptr) {
    std::printf("test_scenario_supervisor_e2e: SKIP (set KAIROS_SUPERVISOR_E2E=1 to run)\n");
    return 0;
  }
  const std::string root = RepoRoot();
  const std::string sim_bin = root + "/core/target/release/kairos-sim";
  const std::string bin_dir = root + "/core/target/release";
  const std::string hubd = root + "/exec/scenario/build/kairos_sim_hubd";
  const std::string trader = root + "/exec/scenario/build/kairos_scenario_trader";
  const std::string supervisor = root + "/exec/scenario/build/kairos_scenario_supervisord";
  const std::string tape = root + "/core/tests/fixtures/tapes/trend_day_2330.kqr";
  for (const std::string& p : {sim_bin, hubd, trader, supervisor, tape}) {
    if (!Exists(p)) {
      std::printf("test_scenario_supervisor_e2e: SKIP (missing %s)\n", p.c_str());
      return 0;
    }
  }

  const std::string tag = std::to_string(::getpid());
  std::filesystem::path base = std::filesystem::temp_directory_path() / ("kairos-sup-e2e-" + tag);
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base);
  const std::string aeron_dir = "/dev/shm/aeron-sup-e2e-" + tag;
  const std::string quote_sock = (base / "q.sock").string();
  const std::string order_sock = (base / "o.sock").string();
  const std::string ctl_sock = (base / "ctl.sock").string();
  const std::string scenario_dir = (base / "scn").string();
  std::filesystem::create_directories(scenario_dir);
  {
    std::ofstream f(scenario_dir + "/2330.toml");
    f << "[scenario]\nname=\"2330\"\nsymbol=\"2330\"\nside=\"Buy\"\nbudget_twd=300000\n"
      << "board=\"OddLot\"\n[pricing]\npolicy=\"cross\"\n[mode]\nlive=false\n";
  }
  const std::string pidfile = aeron_dir + ".kairos-sim.pids";

  int rc = 0;
  // 1) Bring up the isolated sim replaying the fixture tape.
  pid_t sim = SpawnDetached(
      {sim_bin, "replay", tape, "--symbols", "2330", "--speed", "8", "--quote-sock", quote_sock,
       "--order-sock", order_sock, "--aeron-dir", aeron_dir, "--hubd", hubd, "--bin-dir", bin_dir},
      {}, /*own_pgroup=*/true);
  std::printf("e2e: kairos-sim pid=%d\n", sim);
  if (!WaitForPath(quote_sock, 20000)) {
    std::printf("test_scenario_supervisor_e2e: FAIL (sim quote socket never appeared)\n");
    ::kill(-sim, SIGTERM);
    return 1;
  }
  std::printf("e2e: sim quote socket is up\n");

  // 2) Launch the supervisor pointed at the sim sockets.
  pid_t sup = SpawnDetached(
      {supervisor, "--scenario-dir", scenario_dir, "--trader-bin", trader, "--ctl-sock", ctl_sock},
      {{"KAIROS_QUOTE_SOCK", quote_sock}, {"KAIROS_ORDER_SOCK", order_sock}}, false);
  std::printf("e2e: supervisor pid=%d\n", sup);
  if (!WaitForPath(ctl_sock, 5000)) {
    std::printf("test_scenario_supervisor_e2e: FAIL (ctl socket never appeared)\n");
    ::kill(sup, SIGTERM);
    ::kill(-sim, SIGTERM);
    return 1;
  }

  // 3) start 2330 in test mode (stays alive off-hours via --ignore-window).
  std::string r = Request(ctl_sock, R"({"cmd":"start","name":"2330","mode":"test"})");
  std::printf("e2e: start -> %s\n", r.c_str());

  // 4) Poll list until the state advances with cum_fills>0.
  pid_t trader_pid = 0;
  std::string state;
  long cum_fills = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
  while (std::chrono::steady_clock::now() < deadline) {
    std::string list = Request(ctl_sock, R"({"cmd":"list"})");
    state = FieldStr(list, "state");
    cum_fills = FieldLong(list, "cum_fills");
    long pid = FieldLong(list, "pid");
    if (pid > 0) trader_pid = static_cast<pid_t>(pid);
    std::printf("e2e: list state=%s pid=%ld cum_fills=%ld cum_shares=%ld\n", state.c_str(), pid,
                cum_fills, FieldLong(list, "cum_shares"));
    if (cum_fills > 0 && (state == "in-window" || state == "fill-remainder")) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  if (!(cum_fills > 0 && (state == "in-window" || state == "fill-remainder"))) {
    std::printf("test_scenario_supervisor_e2e: FAIL (state never advanced; state=%s fills=%ld)\n",
                state.c_str(), cum_fills);
    rc = 1;
  } else {
    std::printf("e2e: ADVANCED state=%s cum_fills=%ld trader_pid=%d\n", state.c_str(), cum_fills,
                trader_pid);
  }

  // 5) stop 2330 -> stopped.
  std::string stop = Request(ctl_sock, R"({"cmd":"stop","name":"2330"})");
  std::string stop_state = FieldStr(stop, "state");
  std::printf("e2e: stop -> state=%s reason=%s\n", stop_state.c_str(),
              FieldStr(stop, "last_exit_reason").c_str());
  if (stop_state != "stopped") {
    std::printf("test_scenario_supervisor_e2e: FAIL (stop did not reach stopped: %s)\n",
                stop_state.c_str());
    rc = 1;
  }
  if (trader_pid > 0 && !WaitPidGone(trader_pid, 6000)) {
    std::printf("test_scenario_supervisor_e2e: FAIL (trader pid %d still alive after stop)\n",
                trader_pid);
    rc = 1;
  } else if (trader_pid > 0) {
    std::printf("e2e: trader pid %d reaped (ESRCH)\n", trader_pid);
  }

  // 6) Shut the supervisor down; it must leave no orphan trader.
  ::kill(sup, SIGTERM);
  int st = 0;
  ::waitpid(sup, &st, 0);
  std::printf("e2e: supervisor exited\n");
  if (trader_pid > 0 && !(::kill(trader_pid, 0) == -1 && errno == ESRCH)) {
    std::printf("test_scenario_supervisor_e2e: FAIL (orphan trader after supervisor shutdown)\n");
    rc = 1;
  }

  // 7) Tear the sim down and prove its process group is gone.
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
  ::waitpid(sim, &st, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  for (pid_t g : sim_pgids) {
    if (::killpg(g, 0) == 0) {
      std::printf("test_scenario_supervisor_e2e: FAIL (orphan sim pgid %d)\n", g);
      rc = 1;
    }
  }
  std::printf("e2e: sim torn down, %zu pgids checked\n", sim_pgids.size());

  std::filesystem::remove_all(base);
  std::filesystem::remove_all(aeron_dir);
  std::printf("test_scenario_supervisor_e2e: %s\n", rc == 0 ? "OK" : "FAILED");
  return rc;
}
