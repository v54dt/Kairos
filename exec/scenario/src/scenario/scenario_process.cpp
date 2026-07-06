#include "scenario_process.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>

namespace kairos::exec {

namespace {

constexpr std::chrono::seconds kStopGrace{5};

}  // namespace

ProcessManager::ProcessManager(RestartPolicy policy) : policy_(policy) {
  coordinator_ = std::thread([this] { RestartCoordinator(); });
}

ProcessManager::~ProcessManager() { StopAll(); }

bool ProcessManager::IsAlive(const Child& c) { return c.pid > 0 && !c.reaped; }

bool ProcessManager::Spawn(const std::string& name, const std::vector<std::string>& argv,
                           bool live) {
  if (argv.empty()) return false;
  {
    // An operator start clears any crash-restart history for the name and cancels a
    // pending restart; if a restart is mid-spawn, wait for it to land first.
    std::unique_lock<std::mutex> lock(mu_);
    exit_cv_.wait(lock, [&] { return restarting_ != name; });
    RestartState& rs = restart_[name];
    rs.pending = false;
    rs.restart_count = 0;
    rs.gave_up = false;
  }
  restart_cv_.notify_all();
  return DoSpawn(name, argv, live);
}

bool ProcessManager::DoSpawn(const std::string& name, const std::vector<std::string>& argv,
                             bool live) {
  if (argv.empty()) return false;

  // Claim the name under the lock before forking so two concurrent Spawns for the
  // same name cannot both fork and then clobber each other's Child in the map.
  std::unique_ptr<Child> old;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (starting_.count(name) != 0) return false;  // a fork for this name is already in flight
    auto it = children_.find(name);
    if (it != children_.end()) {
      if (IsAlive(*it->second)) return false;  // already running
      old = std::move(it->second);             // reap-and-replace a prior terminal child
      children_.erase(it);
    }
    starting_.insert(name);
  }
  if (old && old->monitor.joinable()) old->monitor.join();
  old.reset();

  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) != 0) {
    std::lock_guard<std::mutex> lock(mu_);
    starting_.erase(name);
    return false;
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    std::lock_guard<std::mutex> lock(mu_);
    starting_.erase(name);
    return false;
  }
  if (pid == 0) {
    // Child: merge stdout+stderr onto the pipe write end, stdin from /dev/null.
    ::close(fds[0]);
    ::dup2(fds[1], STDOUT_FILENO);
    ::dup2(fds[1], STDERR_FILENO);
    if (fds[1] != STDOUT_FILENO && fds[1] != STDERR_FILENO) ::close(fds[1]);
    int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      if (devnull != STDIN_FILENO) ::close(devnull);
    }
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    ::execv(cargv[0], cargv.data());
    ::_exit(127);  // exec failed
  }

  ::close(fds[1]);  // parent keeps the read end only
  auto child = std::make_unique<Child>();
  child->name = name;
  child->pid = pid;
  child->read_fd = fds[0];
  child->live = live;
  child->argv = argv;
  child->spawn_time = std::chrono::steady_clock::now();
  child->state = ScenarioState::kStarting;
  Child* raw = child.get();
  {
    std::lock_guard<std::mutex> lock(mu_);
    children_[name] = std::move(child);  // table entry exists before the monitor runs
    raw->monitor = std::thread([this, raw] { MonitorLoop(raw); });
    starting_.erase(name);
  }
  return true;
}

void ProcessManager::MonitorLoop(Child* c) {
  std::string buf;
  bool dropping = false;  // discarding an over-length line until its newline
  char chunk[4096];
  while (true) {
    ssize_t n = ::read(c->read_fd, chunk, sizeof(chunk));
    if (n <= 0) break;  // EOF (child closed all fds) or error
    for (ssize_t i = 0; i < n; ++i) {
      char ch = chunk[i];
      if (ch == '\n') {
        if (!dropping) {
          std::lock_guard<std::mutex> lock(mu_);
          long now = static_cast<long>(std::time(nullptr));
          c->state = ApplyStdoutLine(c->state, buf, &c->counters, now);
          if (c->state == ScenarioState::kInWindow || c->state == ScenarioState::kFillRemainder)
            c->reached_in_window = true;
          std::string reason = ExtractFailureReason(buf);
          if (!reason.empty()) c->last_fail_reason = reason;
          if (buf.rfind("kairos-exec: end - ", 0) == 0) c->saw_end_line = true;
        }
        buf.clear();
        dropping = false;
        continue;
      }
      if (dropping) continue;
      buf.push_back(ch);
      if (buf.size() > kMaxCtlLineLen) {  // never grow without bound on a runaway line
        buf.clear();
        dropping = true;
      }
    }
  }

  int status = 0;
  ::waitpid(c->pid, &status, 0);  // our own child: returns promptly, no zombie
  {
    std::lock_guard<std::mutex> lock(mu_);
    ExitOutcome o = ClassifyExit(c->requested_stop, status, c->saw_end_line, c->last_fail_reason);
    c->state = o.state;
    c->last_exit_reason = o.reason;
    c->reaped = true;
    ::close(c->read_fd);
    c->read_fd = -1;
    MaybeScheduleRestart(c);
  }
  exit_cv_.notify_all();
  restart_cv_.notify_all();
}

void ProcessManager::MaybeScheduleRestart(Child* c) {
  // Restart ONLY a genuine crash of an owned child; never a clean close, an
  // operator stop, or anything during shutdown.
  if (c->state != ScenarioState::kCrashed || c->requested_stop || shutting_down_) return;
  RestartState& rs = restart_[c->name];
  auto uptime = std::chrono::steady_clock::now() - c->spawn_time;
  if (c->reached_in_window || uptime >= policy_.healthy_reset) rs.restart_count = 0;
  if (rs.restart_count >= policy_.max_retries) {
    rs.gave_up = true;
    c->last_exit_reason =
        "crashed; gave up after " + std::to_string(rs.restart_count) + " restarts";
    return;
  }
  auto delay = policy_.base_delay;
  for (int i = 0; i < rs.restart_count && delay < policy_.max_delay; ++i) delay *= 2;
  if (delay > policy_.max_delay) delay = policy_.max_delay;
  rs.pending = true;
  rs.deadline = std::chrono::steady_clock::now() + delay;
  rs.argv = c->argv;
  rs.live = c->live;
}

void ProcessManager::RestartCoordinator() {
  std::unique_lock<std::mutex> lock(mu_);
  while (!shutting_down_) {
    auto next = std::chrono::steady_clock::time_point::max();
    std::string ready;
    auto now = std::chrono::steady_clock::now();
    for (auto& [name, rs] : restart_) {
      if (!rs.pending) continue;
      if (rs.deadline <= now) {
        ready = name;
        break;
      }
      next = std::min(next, rs.deadline);
    }
    if (!ready.empty()) {
      // Commit the fire under the same lock hold that cleared pending, so a
      // concurrent stop either cancels first or waits behind restarting_.
      RestartState& rs = restart_[ready];
      rs.pending = false;
      ++rs.restart_count;
      std::vector<std::string> argv = rs.argv;
      bool live = rs.live;
      restarting_ = ready;
      lock.unlock();
      DoSpawn(ready, argv, live);
      lock.lock();
      restarting_.clear();
      exit_cv_.notify_all();
      continue;
    }
    if (next == std::chrono::steady_clock::time_point::max())
      restart_cv_.wait(lock);
    else
      restart_cv_.wait_until(lock, next);
  }
}

void ProcessManager::StopChild(const std::string& name) {
  pid_t pid = -1;
  {
    std::unique_lock<std::mutex> lock(mu_);
    // If a restart is mid-spawn, wait for it to land so we can stop the live child.
    exit_cv_.wait(lock, [&] { return restarting_ != name; });
    auto rit = restart_.find(name);
    if (rit != restart_.end()) rit->second.pending = false;  // cancel a pending restart
    auto it = children_.find(name);
    if (it == children_.end()) {
      lock.unlock();
      restart_cv_.notify_all();
      return;
    }
    if (!IsAlive(*it->second)) {
      // Terminal child (e.g. crashed awaiting a now-cancelled restart): mark stopped.
      if (it->second->state == ScenarioState::kCrashed) {
        it->second->state = ScenarioState::kStopped;
        it->second->last_exit_reason = "stopped by operator";
      }
      lock.unlock();
      restart_cv_.notify_all();
      return;
    }
    it->second->requested_stop = true;
    it->second->state = ScenarioState::kStopping;
    pid = it->second->pid;
  }
  restart_cv_.notify_all();
  ::kill(pid, SIGINT);
  {
    std::unique_lock<std::mutex> lock(mu_);
    auto reaped = [&] {
      auto it = children_.find(name);
      return it == children_.end() || it->second->reaped;
    };
    if (!exit_cv_.wait_for(lock, kStopGrace, reaped)) {
      ::kill(pid, SIGKILL);  // ignored the SIGINT: force it, the monitor still reaps
      exit_cv_.wait(lock, reaped);
    }
  }
}

void ProcessManager::StopAll() {
  // Stop the restart coordinator FIRST so no restart can spawn a child mid-shutdown;
  // any child it spawned just before exiting is already in children_ and gets reaped.
  {
    std::lock_guard<std::mutex> lock(mu_);
    shutting_down_ = true;
    for (auto& [name, rs] : restart_) rs.pending = false;
  }
  restart_cv_.notify_all();
  if (coordinator_.joinable()) coordinator_.join();

  std::vector<pid_t> pids;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [name, c] : children_) {
      if (!IsAlive(*c)) continue;
      c->requested_stop = true;
      c->state = ScenarioState::kStopping;
      pids.push_back(c->pid);
    }
  }
  for (pid_t pid : pids) ::kill(pid, SIGINT);

  {
    std::unique_lock<std::mutex> lock(mu_);
    auto all_reaped = [&] {
      for (auto& [name, c] : children_)
        if (!c->reaped && c->pid > 0) return false;
      return true;
    };
    if (!exit_cv_.wait_for(lock, kStopGrace, all_reaped)) {
      for (auto& [name, c] : children_)
        if (!c->reaped && c->pid > 0) ::kill(c->pid, SIGKILL);
      exit_cv_.wait(lock, all_reaped);
    }
  }

  std::map<std::string, std::unique_ptr<Child>> drained;
  {
    std::lock_guard<std::mutex> lock(mu_);
    drained.swap(children_);
  }
  for (auto& [name, c] : drained)
    if (c->monitor.joinable()) c->monitor.join();
}

ProcessManager::ChildStatus ProcessManager::StatusOf(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mu_);
  ChildStatus s;
  auto it = children_.find(name);
  if (it == children_.end()) return s;
  const Child& c = *it->second;
  s.present = true;
  s.state = c.state;
  s.pid = c.reaped ? 0 : static_cast<long>(c.pid);
  s.counters = c.counters;
  s.last_exit_reason = c.last_exit_reason;
  s.live = c.live;
  auto rit = restart_.find(name);
  if (rit != restart_.end()) {
    s.restart_count = rit->second.restart_count;
    s.gave_up = rit->second.gave_up;
  }
  return s;
}

std::vector<std::string> ProcessManager::Names() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::string> names;
  names.reserve(children_.size());
  for (const auto& [name, c] : children_) names.push_back(name);
  return names;
}

}  // namespace kairos::exec
