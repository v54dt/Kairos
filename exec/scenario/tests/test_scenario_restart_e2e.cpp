// End-to-end proof of crash-restart against the REAL kairos_scenario_supervisord
// binary (no sim needed): the trader-bin is a wrapper shell script that crashes
// (exits nonzero), so the supervisor's own fork/exec/monitor/reap path drives the
// restart. It exercises two scenarios over the real UDS control socket:
//   (1) crash-loop: restart_count climbs with the injected backoff, then the cap
//       gives up into a terminal crashed state and stops restarting;
//   (2) a stop DURING the backoff wait cancels the pending restart.
// Restart policy is injected via KAIROS_RESTART_* env so the whole run is sub-second.
// The supervisor runs in its own process group; at the end we prove that group is
// gone (no orphan trader / no orphan supervisor).
//
// Self-skips (exit 0) if the supervisord binary is not next to this test binary.

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

std::string SelfDir() {
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  buf[n] = '\0';
  return std::filesystem::path(buf).parent_path().string();
}

bool Exists(const std::string& p) { return std::filesystem::exists(p); }

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
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
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

bool FieldBool(const std::string& json, const std::string& key) {
  std::string k = "\"" + key + "\":";
  std::size_t at = json.find(k);
  if (at == std::string::npos) return false;
  return json.compare(at + k.size(), 4, "true") == 0;
}

int CountLines(const std::string& path) {
  std::ifstream f(path);
  int n = 0;
  std::string line;
  while (std::getline(f, line))
    if (!line.empty()) ++n;
  return n;
}

pid_t SpawnSupervisor(const std::string& bin, const std::vector<std::string>& argv,
                      const std::vector<std::pair<std::string, std::string>>& env) {
  pid_t pid = ::fork();
  if (pid != 0) return pid;
  ::setpgid(0, 0);  // own group so we can prove the whole tree dies (no orphan)
  for (const auto& [k, v] : env) ::setenv(k.c_str(), v.c_str(), 1);
  std::vector<char*> cargv;
  cargv.push_back(const_cast<char*>(bin.c_str()));
  for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
  cargv.push_back(nullptr);
  ::execv(bin.c_str(), cargv.data());
  ::_exit(127);
}

}  // namespace

int main() {
  const std::string bin = SelfDir() + "/kairos_scenario_supervisord";
  if (!Exists(bin)) {
    std::printf("test_scenario_restart_e2e: SKIP (missing %s)\n", bin.c_str());
    return 0;
  }

  const std::string tag = std::to_string(::getpid());
  std::filesystem::path base =
      std::filesystem::temp_directory_path() / ("kairos-restart-e2e-" + tag);
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base);
  const std::string scenario_dir = (base / "scn").string();
  std::filesystem::create_directories(scenario_dir);
  {
    std::ofstream f(scenario_dir + "/2330.toml");
    f << "[scenario]\nname=\"2330\"\nsymbol=\"2330\"\nside=\"Buy\"\nbudget_twd=300000\n"
      << "board=\"OddLot\"\n[pricing]\npolicy=\"cross\"\n[mode]\nlive=false\n";
  }

  int rc = 0;

  // ---- Scenario 1: crash-loop hits the cap and gives up ---------------------
  {
    const std::string ctl = (base / "ctl1.sock").string();
    const std::string runs = (base / "runs1.log").string();
    const std::string crasher = (base / "crasher1.sh").string();
    {
      std::ofstream f(crasher);
      f << "#!/bin/sh\ndate +%s.%N >> '" << runs << "'\nexit 7\n";
    }
    ::chmod(crasher.c_str(), 0755);

    pid_t sup = SpawnSupervisor(
        bin, {"--scenario-dir", scenario_dir, "--trader-bin", crasher, "--ctl-sock", ctl},
        {{"KAIROS_RESTART_BASE_MS", "60"},
         {"KAIROS_RESTART_MAX_MS", "120"},
         {"KAIROS_RESTART_MAX_RETRIES", "3"},
         {"KAIROS_RESTART_HEALTHY_MS", "100000"}});
    std::printf("e2e: supervisor(1) pid=%d pgid=%d\n", sup, sup);
    if (!WaitForPath(ctl, 5000)) {
      std::printf("test_scenario_restart_e2e: FAIL (ctl1 never appeared)\n");
      ::kill(-sup, SIGKILL);
      return 1;
    }
    std::printf("e2e: start -> %s\n",
                Request(ctl, R"({"cmd":"start","name":"2330","mode":"test"})").c_str());

    // Poll until it gives up.
    std::string state;
    bool gave_up = false;
    long rcount = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
      std::string list = Request(ctl, R"({"cmd":"list"})");
      state = FieldStr(list, "state");
      gave_up = FieldBool(list, "gave_up");
      rcount = FieldLong(list, "restart_count");
      if (gave_up) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    int run_count = CountLines(runs);
    std::printf("e2e: gave_up=%d state=%s restart_count=%ld runs=%d\n", gave_up, state.c_str(),
                rcount, run_count);
    if (!(gave_up && state == "crashed" && rcount == 3 && run_count == 4)) {
      std::printf("test_scenario_restart_e2e: FAIL (crash-loop did not give up cleanly)\n");
      rc = 1;
    }
    // No more restarts after giving up.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (CountLines(runs) != run_count) {
      std::printf("test_scenario_restart_e2e: FAIL (restart fired after give-up)\n");
      rc = 1;
    }

    ::kill(-sup, SIGTERM);
    int st = 0;
    ::waitpid(sup, &st, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (::killpg(sup, 0) == 0) {
      std::printf("test_scenario_restart_e2e: FAIL (orphan supervisor(1) pgid %d)\n", sup);
      rc = 1;
    } else {
      std::printf("e2e: supervisor(1) pgroup gone (no orphan)\n");
    }
  }

  // ---- Scenario 2: a stop DURING backoff cancels the pending restart --------
  {
    const std::string ctl = (base / "ctl2.sock").string();
    const std::string runs = (base / "runs2.log").string();
    const std::string crasher = (base / "crasher2.sh").string();
    {
      std::ofstream f(crasher);
      f << "#!/bin/sh\ndate +%s.%N >> '" << runs << "'\nexit 7\n";
    }
    ::chmod(crasher.c_str(), 0755);

    pid_t sup = SpawnSupervisor(
        bin, {"--scenario-dir", scenario_dir, "--trader-bin", crasher, "--ctl-sock", ctl},
        {{"KAIROS_RESTART_BASE_MS", "1500"},  // long backoff to stop within
         {"KAIROS_RESTART_MAX_MS", "1500"},
         {"KAIROS_RESTART_MAX_RETRIES", "5"},
         {"KAIROS_RESTART_HEALTHY_MS", "100000"}});
    std::printf("e2e: supervisor(2) pid=%d\n", sup);
    if (!WaitForPath(ctl, 5000)) {
      std::printf("test_scenario_restart_e2e: FAIL (ctl2 never appeared)\n");
      ::kill(-sup, SIGKILL);
      return 1;
    }
    Request(ctl, R"({"cmd":"start","name":"2330","mode":"test"})");

    // Wait until it has crashed once and is now in the (1.5s) backoff.
    std::string state;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      state = FieldStr(Request(ctl, R"({"cmd":"list"})"), "state");
      if (state == "crashed") break;
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    int runs_at_stop = CountLines(runs);
    std::printf("e2e: crashed; runs=%d; sending stop during backoff\n", runs_at_stop);
    std::string stop = Request(ctl, R"({"cmd":"stop","name":"2330"})");
    std::string stop_state = FieldStr(stop, "state");
    // Wait well past the backoff deadline; the cancelled restart must not fire.
    std::this_thread::sleep_for(std::chrono::milliseconds(2200));
    int runs_after = CountLines(runs);
    std::printf("e2e: stop_state=%s runs_before=%d runs_after=%d\n", stop_state.c_str(),
                runs_at_stop, runs_after);
    if (!(stop_state == "stopped" && runs_after == runs_at_stop)) {
      std::printf("test_scenario_restart_e2e: FAIL (stop during backoff did not cancel)\n");
      rc = 1;
    }

    ::kill(-sup, SIGTERM);
    int st = 0;
    ::waitpid(sup, &st, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (::killpg(sup, 0) == 0) {
      std::printf("test_scenario_restart_e2e: FAIL (orphan supervisor(2) pgid %d)\n", sup);
      rc = 1;
    } else {
      std::printf("e2e: supervisor(2) pgroup gone (no orphan)\n");
    }
  }

  // ---- Scenario 3: an in-window crash-loop still gives up (no cap bypass) ----
  // The trader reaches in-window (prints a fill) then crashes immediately. Reaching
  // in-window must NOT reset the retry counter, so the cap still fires.
  {
    const std::string ctl = (base / "ctl3.sock").string();
    const std::string runs = (base / "runs3.log").string();
    const std::string crasher = (base / "crasher3.sh").string();
    {
      std::ofstream f(crasher);
      f << "#!/bin/sh\ndate +%s.%N >> '" << runs << "'\n"
        << "printf 'kairos-exec: Buy 2330 NT$ 300000, cross, PAPER\\n'\n"
        << "printf 'kairos-exec: fill k-1 1000 @ 925.00  (cum 1000 sh)\\n'\nexit 9\n";
    }
    ::chmod(crasher.c_str(), 0755);

    pid_t sup = SpawnSupervisor(
        bin, {"--scenario-dir", scenario_dir, "--trader-bin", crasher, "--ctl-sock", ctl},
        {{"KAIROS_RESTART_BASE_MS", "60"},
         {"KAIROS_RESTART_MAX_MS", "120"},
         {"KAIROS_RESTART_MAX_RETRIES", "3"},
         {"KAIROS_RESTART_HEALTHY_MS", "100000"}});
    std::printf("e2e: supervisor(3) pid=%d\n", sup);
    if (!WaitForPath(ctl, 5000)) {
      std::printf("test_scenario_restart_e2e: FAIL (ctl3 never appeared)\n");
      ::kill(-sup, SIGKILL);
      return 1;
    }
    Request(ctl, R"({"cmd":"start","name":"2330","mode":"live"})");

    std::string state;
    bool gave_up = false;
    long rcount = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
      std::string list = Request(ctl, R"({"cmd":"list"})");
      state = FieldStr(list, "state");
      gave_up = FieldBool(list, "gave_up");
      rcount = FieldLong(list, "restart_count");
      if (gave_up) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    int run_count = CountLines(runs);
    std::printf("e2e: in-window loop gave_up=%d state=%s restart_count=%ld runs=%d\n", gave_up,
                state.c_str(), rcount, run_count);
    if (!(gave_up && state == "crashed" && rcount == 3 && run_count == 4)) {
      std::printf("test_scenario_restart_e2e: FAIL (in-window crash-loop bypassed the cap)\n");
      rc = 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (CountLines(runs) != run_count) {
      std::printf("test_scenario_restart_e2e: FAIL (in-window restart fired after give-up)\n");
      rc = 1;
    }

    ::kill(-sup, SIGTERM);
    int st = 0;
    ::waitpid(sup, &st, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (::killpg(sup, 0) == 0) {
      std::printf("test_scenario_restart_e2e: FAIL (orphan supervisor(3) pgid %d)\n", sup);
      rc = 1;
    } else {
      std::printf("e2e: supervisor(3) pgroup gone (no orphan)\n");
    }
  }

  std::filesystem::remove_all(base);
  std::printf("test_scenario_restart_e2e: %s\n", rc == 0 ? "OK" : "FAILED");
  return rc;
}
