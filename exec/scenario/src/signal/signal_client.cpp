#include "signal_client.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

namespace kairos::exec {

namespace {
constexpr auto kBackoffMin = std::chrono::milliseconds(200);
constexpr auto kBackoffMax = std::chrono::seconds(5);
constexpr int kPollTickMs = 20;  // wake often enough to re-check the hb clock

void InterruptibleSleep(std::chrono::milliseconds d, const std::atomic<bool>& stop) {
  auto until = std::chrono::steady_clock::now() + d;
  while (!stop && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

bool WriteAll(int fd, const std::string& s) {
  std::size_t off = 0;
  while (off < s.size()) {
    ssize_t w = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
    if (w <= 0) return false;
    off += static_cast<std::size_t>(w);
  }
  return true;
}
}  // namespace

SignalClient::SignalClient(std::string socket_path, SignalSubscribe sub, SignalCallbacks cb,
                           std::chrono::milliseconds hb_interval, SignalClock clock)
    : socket_path_(std::move(socket_path)),
      sub_(std::move(sub)),
      cb_(std::move(cb)),
      hb_interval_(hb_interval),
      clock_(std::move(clock)) {}

SignalClient::~SignalClient() { Stop(); }

void SignalClient::Start() {
  thread_ = std::thread([this] { Run(); });
}

void SignalClient::Stop() {
  stop_ = true;
  int fd = fd_.exchange(-1);
  if (fd >= 0) ::shutdown(fd, SHUT_RDWR);  // unblock a pending poll/read
  if (thread_.joinable()) thread_.join();
  if (fd >= 0) ::close(fd);  // Stop() stole the fd from Run(), so it owns the close
}

int SignalClient::ConnectAndSubscribe() {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (errno != EINPROGRESS) {
      ::close(fd);
      return -1;
    }
    pollfd pfd{fd, POLLOUT, 0};
    bool connected = false;
    while (!stop_ && !connected) {
      int p = ::poll(&pfd, 1, 100);
      if (p < 0) {
        if (errno == EINTR) continue;
        ::close(fd);
        return -1;
      }
      if (p == 0) continue;
      int err = 0;
      socklen_t len = sizeof(err);
      ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
      if (err != 0) {
        ::close(fd);
        return -1;
      }
      connected = true;
    }
    if (!connected) {
      ::close(fd);
      return -1;
    }
  }
  ::fcntl(fd, F_SETFL, flags);  // restore blocking for the subscribe write

  fd_ = fd;  // publish before the blocking write so Stop() can shutdown() it
  if (!WriteAll(fd, SerializeSubscribe(sub_))) {
    fd_.store(-1);
    ::close(fd);
    return -1;
  }
  return fd;
}

void SignalClient::Run() {
  auto backoff = kBackoffMin;
  while (!stop_) {
    int fd = ConnectAndSubscribe();
    if (fd < 0) {
      InterruptibleSleep(backoff, stop_);
      backoff =
          std::min(backoff * 2, std::chrono::duration_cast<std::chrono::milliseconds>(kBackoffMax));
      continue;
    }
    backoff = kBackoffMin;
    {
      std::lock_guard<std::mutex> lock(mu_);
      last_hb_ = clock_.mono();  // grace start; health persists across reconnects
      have_seq_ = false;
    }
    ReadLoop(fd);
    int old = fd_.exchange(-1);
    if (old >= 0) ::close(old);
  }
}

void SignalClient::ReadLoop(int fd) {
  std::string buf;
  bool dropping = false;  // discarding an over-length line until its newline
  char chunk[4096];
  while (!stop_) {
    pollfd pfd{fd, POLLIN, 0};
    int p = ::poll(&pfd, 1, kPollTickMs);
    if (stop_) break;
    if (p < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (CheckLiveness()) break;  // heartbeat timeout -> reconnect
    if (p == 0) continue;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
    ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0) break;  // EOF or error
    for (ssize_t i = 0; i < n; ++i) {
      char ch = chunk[i];
      if (ch == '\n') {
        if (!dropping) HandleLine(buf);
        dropping = false;
        buf.clear();
        continue;
      }
      if (dropping) continue;
      buf.push_back(ch);
      if (buf.size() > kMaxSignalLineLen) {  // never buffer a runaway line unbounded
        buf.clear();
        dropping = true;
      }
    }
  }
  if (!stop_) MarkLostFromError();
}

void SignalClient::HandleLine(const std::string& line) {
  ServerFrame f;
  std::string err;
  if (!ParseServerFrame(line, &f, &err)) return;  // fail-closed: drop rejects
  if (f.type == ServerFrameType::kSignal) {
    HandleSeq(f.push.seq);
    if (cb_.on_signal) cb_.on_signal(f.push);
  } else if (f.type == ServerFrameType::kHeartbeat) {
    HandleSeq(f.hb.seq);
    OnHeartbeat();
  }
}

void SignalClient::HandleSeq(std::uint64_t seq) {
  bool gap = false;
  std::uint64_t expected = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (have_seq_ && seq != last_seq_ + 1) {
      gap = true;
      expected = last_seq_ + 1;
    }
    last_seq_ = seq;
    have_seq_ = true;
  }
  if (gap && cb_.on_seq_gap) cb_.on_seq_gap(expected, seq);
}

void SignalClient::OnHeartbeat() {
  bool fire_restored = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    last_hb_ = clock_.mono();
    if (health_ == Health::kInitial) {
      health_ = Health::kHealthy;
    } else if (health_ == Health::kLost) {
      health_ = Health::kHealthy;
      fire_restored = true;
    }
  }
  if (fire_restored && cb_.on_signal_restored) cb_.on_signal_restored();
}

void SignalClient::MarkLostFromError() {
  bool fire = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (health_ == Health::kHealthy) {
      health_ = Health::kLost;
      fire = true;
    }
  }
  if (fire && cb_.on_signal_lost) cb_.on_signal_lost("socket error");
}

bool SignalClient::CheckLiveness() {
  bool fire_lost = false;
  bool teardown = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (health_ == Health::kHealthy && clock_.mono() - last_hb_ >= 3 * hb_interval_) {
      health_ = Health::kLost;
      fire_lost = true;
      teardown = true;
    } else if (health_ == Health::kInitial && clock_.mono() - last_hb_ >= 3 * hb_interval_) {
      teardown = true;  // never became healthy: reconnect silently
    }
  }
  if (fire_lost && cb_.on_signal_lost) cb_.on_signal_lost("heartbeat timeout");
  return teardown;
}

}  // namespace kairos::exec
