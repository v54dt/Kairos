#include "scenario_ctl_server.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_set>
#include <utility>

#include "scenario_state.h"

namespace kairos::exec {

namespace {

void WriteAll(int fd, const std::string& s) {
  std::size_t off = 0;
  while (off < s.size()) {
    ssize_t w = ::write(fd, s.data() + off, s.size() - off);
    if (w <= 0) return;  // closed or timed out: drop, ClientLoop will detect EOF
    off += static_cast<std::size_t>(w);
  }
}

}  // namespace

Supervisor::Supervisor(std::string scenario_dir, std::string trader_bin, RestartPolicy policy)
    : scenario_dir_(std::move(scenario_dir)), trader_bin_(std::move(trader_bin)), pm_(policy) {}

std::vector<ScenarioSnapshotRow> Supervisor::Snapshot() {
  std::vector<ScenarioInfo> scenarios = EnumerateScenarios(scenario_dir_);
  std::unordered_set<std::string> seen;
  std::vector<ScenarioSnapshotRow> rows;
  auto add = [&](const std::string& name) {
    if (!seen.insert(name).second) return;
    ScenarioSnapshotRow row;
    row.name = name;
    ProcessManager::ChildStatus st = pm_.StatusOf(name);
    if (st.present) {
      row.state = StateName(st.state);
      row.pid = st.pid;
      row.cum_fills = st.counters.cum_fills;
      row.cum_shares = st.counters.cum_shares;
      row.last_fill_ts = st.counters.last_fill_ts;
      row.last_exit_reason = st.last_exit_reason;
      row.live = st.live;
      row.restart_count = st.restart_count;
      row.gave_up = st.gave_up;
    } else {
      row.state = StateName(ScenarioState::kStopped);
    }
    rows.push_back(std::move(row));
  };
  for (const ScenarioInfo& s : scenarios) add(s.name);
  for (const std::string& owned : pm_.Names()) add(owned);  // e.g. a since-removed toml
  std::sort(
      rows.begin(), rows.end(),
      [](const ScenarioSnapshotRow& a, const ScenarioSnapshotRow& b) { return a.name < b.name; });
  return rows;
}

std::string Supervisor::Handle(const ScenarioRequest& req) {
  if (req.cmd == ScenarioCmd::kList) {
    return SerializeScenarioSnapshot(true, "", Snapshot());
  }
  if (req.cmd == ScenarioCmd::kStop) {
    pm_.StopChild(req.name);
    return SerializeScenarioSnapshot(true, "", Snapshot());
  }
  // start: resolve the scenario toml, build the per-mode argv, spawn+own it.
  std::vector<ScenarioInfo> scenarios = EnumerateScenarios(scenario_dir_);
  const ScenarioInfo* info = nullptr;
  for (const ScenarioInfo& s : scenarios)
    if (s.name == req.name) info = &s;
  if (info == nullptr) {
    return SerializeScenarioSnapshot(false, "unknown scenario", Snapshot());
  }
  std::vector<std::string> argv = BuildTraderArgv(trader_bin_, info->path, req.mode);
  bool live = req.mode == ScenarioMode::kLive;
  if (!pm_.Spawn(req.name, argv, live)) {
    return SerializeScenarioSnapshot(false, "already running or spawn failed", Snapshot());
  }
  return SerializeScenarioSnapshot(true, "", Snapshot());
}

std::string Supervisor::HandleLine(const std::string& line) {
  ScenarioRequest req;
  std::string err;
  if (!ParseScenarioRequest(line, &req, &err)) {
    return SerializeScenarioSnapshot(false, err, Snapshot());  // fail-closed, no spawn
  }
  return Handle(req);
}

void Supervisor::StopAll() { pm_.StopAll(); }

ScenarioCtlServer::ScenarioCtlServer(Supervisor* sup, std::string path)
    : sup_(sup), path_(std::move(path)) {}

ScenarioCtlServer::~ScenarioCtlServer() { Stop(); }

bool ScenarioCtlServer::Start() {
  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return false;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
  ::unlink(path_.c_str());  // clear a stale socket
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(listen_fd_, 16) != 0) {
    std::fprintf(stderr, "kairos-scenario-supervisor: bind/listen %s failed\n", path_.c_str());
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  accept_thread_ = std::thread([this] { AcceptLoop(); });
  std::printf("kairos-scenario-supervisor: listening on %s\n", path_.c_str());
  std::fflush(stdout);
  return true;
}

void ScenarioCtlServer::AcceptLoop() {
  while (!stop_) {
    int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (stop_) break;
      continue;
    }
    timeval tv{5, 0};  // bound writes: a stuck reader can't wedge the server
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    {
      std::lock_guard<std::mutex> lock(clients_mu_);
      live_.insert(fd);
    }
    ++active_clients_;
    std::thread([this, fd] {
      ClientLoop(fd);
      --active_clients_;
    }).detach();
  }
}

void ScenarioCtlServer::ClientLoop(int fd) {
  std::string buf;
  bool dropping = false;  // discarding an over-length line until its newline
  char chunk[4096];
  while (!stop_) {
    ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0) break;  // EOF or error
    for (ssize_t i = 0; i < n; ++i) {
      char ch = chunk[i];
      if (ch == '\n') {
        if (dropping) {
          WriteAll(fd, SerializeScenarioSnapshot(false, "line too long", {}));
          dropping = false;
        } else {
          WriteAll(fd, sup_->HandleLine(buf));
        }
        buf.clear();
        continue;
      }
      if (dropping) continue;
      buf.push_back(ch);
      if (buf.size() > kMaxCtlLineLen) {  // never buffer a runaway line unbounded
        buf.clear();
        dropping = true;
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(clients_mu_);
    live_.erase(fd);
  }
  ::close(fd);
}

void ScenarioCtlServer::Stop() {
  if (stop_.exchange(true)) return;  // idempotent
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (accept_thread_.joinable()) accept_thread_.join();

  std::vector<int> fds;
  {
    std::lock_guard<std::mutex> lock(clients_mu_);
    fds.assign(live_.begin(), live_.end());
  }
  for (int fd : fds) ::shutdown(fd, SHUT_RDWR);  // unblock pending reads
  while (active_clients_.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ::unlink(path_.c_str());
}

}  // namespace kairos::exec
