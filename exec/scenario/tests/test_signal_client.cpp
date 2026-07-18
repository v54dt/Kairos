// Signal client against a controllable fake UDS server, driven by an injected
// clock. Covers: subscribe sent, signal dispatch, heartbeat-miss -> lost exactly
// once, drop -> reconnect -> restored + re-subscribe, seq gap observed, and a
// clean Stop() under a signal flood. Deterministic (fake clock for liveness); the
// small real poll tick only bounds how fast the reader notices.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "signal_client.h"
#include "signal_proto.h"
#include "test_check.h"

using namespace kairos::exec;
using namespace std::chrono_literals;

namespace {

template <class Pred>
bool WaitFor(Pred p, std::chrono::milliseconds timeout = 5s) {
  auto end = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < end) {
    if (p()) return true;
    std::this_thread::sleep_for(2ms);
  }
  return p();
}

// A test-owned monotonic clock. Liveness reads only this, so the test decides
// when a heartbeat is "missed" by advancing it.
struct FakeClock {
  std::atomic<long long> ns{0};
  SignalClock Make() {
    return SignalClock{[this] {
      return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ns.load()));
    }};
  }
  void Advance(std::chrono::milliseconds d) {
    ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
  }
};

// Fake signal daemon: keeps accepting, records every subscribe line, and lets the
// test push frames to (or drop) the current connection.
class FakeServer {
 public:
  explicit FakeServer(std::string path) : path_(std::move(path)) {}
  ~FakeServer() { Stop(); }

  bool Start() {
    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
    ::unlink(path_.c_str());
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listen_fd_, 8) != 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return false;
    }
    accept_thread_ = std::thread([this] { AcceptLoop(); });
    return true;
  }

  void Stop() {
    if (stop_.exchange(true)) return;
    if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);  // break accept()
    if (accept_thread_.joinable()) accept_thread_.join();
    if (listen_fd_ >= 0) {  // safe now: the accept thread is joined
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      for (int fd : active_) ::shutdown(fd, SHUT_RDWR);
    }
    for (std::thread& t : readers_)
      if (t.joinable()) t.join();
    ::unlink(path_.c_str());
  }

  int ConnCount() {
    std::lock_guard<std::mutex> lock(mu_);
    return conn_count_;
  }

  std::vector<std::string> Subscribes() {
    std::lock_guard<std::mutex> lock(mu_);
    return subs_;
  }

  bool SendSignal(const SignalPush& p) { return SendRaw(SerializeSignal(p)); }
  bool SendHeartbeat(const SignalHeartbeat& h) { return SendRaw(SerializeHeartbeat(h)); }

  void DropCurrent() {
    std::lock_guard<std::mutex> lock(mu_);
    if (cur_fd_ >= 0) ::shutdown(cur_fd_, SHUT_RDWR);  // reader owns the close
  }

 private:
  bool SendRaw(const std::string& s) {
    std::lock_guard<std::mutex> lock(mu_);
    if (cur_fd_ < 0) return false;
    std::size_t off = 0;
    while (off < s.size()) {
      ssize_t w = ::send(cur_fd_, s.data() + off, s.size() - off, MSG_NOSIGNAL);
      if (w <= 0) return false;
      off += static_cast<std::size_t>(w);
    }
    return true;
  }

  void AcceptLoop() {
    while (!stop_) {
      int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) {
        if (stop_) break;
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(mu_);
        cur_fd_ = fd;
        active_.insert(fd);
        ++conn_count_;
        readers_.emplace_back([this, fd] { ReaderLoop(fd); });
      }
    }
  }

  void ReaderLoop(int fd) {
    std::string buf;
    char chunk[4096];
    while (!stop_) {
      ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
      if (n <= 0) break;
      for (ssize_t i = 0; i < n; ++i) {
        if (chunk[i] == '\n') {
          std::lock_guard<std::mutex> lock(mu_);
          subs_.push_back(buf);
          buf.clear();
        } else {
          buf.push_back(chunk[i]);
        }
      }
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (cur_fd_ == fd) cur_fd_ = -1;
    active_.erase(fd);
    ::close(fd);
  }

  std::string path_;
  int listen_fd_ = -1;
  std::thread accept_thread_;
  std::atomic<bool> stop_{false};

  std::mutex mu_;
  int cur_fd_ = -1;
  int conn_count_ = 0;
  std::set<int> active_;
  std::vector<std::string> subs_;
  std::vector<std::thread> readers_;
};

std::string TempPath(const char* tag) {
  return "/tmp/kairos-test-signal-" + std::to_string(::getpid()) + "-" + tag + ".sock";
}

SignalSubscribe MakeSub() { return SignalSubscribe{"break-2330", "2330"}; }

// (a) subscribe sent, (b) signal dispatched.
void TestSubscribeAndSignal() {
  FakeServer srv(TempPath("sub"));
  CHECK(srv.Start());
  FakeClock clk;
  std::atomic<int> signals{0};
  std::string got_symbol;
  std::mutex gm;
  SignalCallbacks cb;
  cb.on_signal = [&](const SignalPush& p) {
    {
      std::lock_guard<std::mutex> l(gm);
      got_symbol = p.symbol;
    }
    ++signals;
  };
  SignalClient cli(TempPath("sub"), MakeSub(), cb, 1000ms, clk.Make());
  cli.Start();
  CHECK(WaitFor([&] { return srv.ConnCount() >= 1; }));
  CHECK(WaitFor([&] { return !srv.Subscribes().empty(); }));
  CHECK(srv.Subscribes()[0] + "\n" == SerializeSubscribe(MakeSub()));

  SignalPush p;
  p.signal = "break-2330";
  p.symbol = "2330";
  p.action = SignalAction::kEnter;
  p.seq = 1;
  p.ts_us = 123;
  CHECK(srv.SendSignal(p));
  CHECK(WaitFor([&] { return signals.load() >= 1; }));
  {
    std::lock_guard<std::mutex> l(gm);
    CHECK(got_symbol == "2330");
  }
  cli.Stop();
}

// (c) first hb -> healthy, silence + clock advance -> lost exactly once.
void TestHeartbeatMissLostOnce() {
  FakeServer srv(TempPath("hbm"));
  CHECK(srv.Start());
  FakeClock clk;
  std::atomic<int> lost{0}, signals{0};
  SignalCallbacks cb;
  cb.on_signal = [&](const SignalPush&) { ++signals; };
  cb.on_signal_lost = [&](const std::string&) { ++lost; };
  SignalClient cli(TempPath("hbm"), MakeSub(), cb, 1000ms, clk.Make());
  cli.Start();
  CHECK(WaitFor([&] { return srv.ConnCount() >= 1; }));

  SignalHeartbeat hb;
  hb.seq = 1;
  hb.ts_us = 1;
  CHECK(srv.SendHeartbeat(hb));
  SignalPush p;  // in-order after the hb: OnSignal proves the hb was processed
  p.signal = "break-2330";
  p.symbol = "2330";
  p.action = SignalAction::kEnter;
  p.seq = 2;
  p.ts_us = 2;
  CHECK(srv.SendSignal(p));
  CHECK(WaitFor([&] { return signals.load() >= 1; }));
  CHECK(lost.load() == 0);  // healthy, no miss yet

  clk.Advance(3001ms);  // past 3 * 1000ms since the last heartbeat
  CHECK(WaitFor([&] { return lost.load() >= 1; }));
  clk.Advance(10000ms);
  std::this_thread::sleep_for(150ms);
  CHECK(lost.load() == 1);  // exactly once, even as the clock keeps advancing
  cli.Stop();
}

// (d) drop -> reconnect -> restored + re-subscribe.
void TestReconnectRestored() {
  FakeServer srv(TempPath("rec"));
  CHECK(srv.Start());
  FakeClock clk;
  std::atomic<int> lost{0}, restored{0}, signals{0};
  SignalCallbacks cb;
  cb.on_signal = [&](const SignalPush&) { ++signals; };
  cb.on_signal_lost = [&](const std::string&) { ++lost; };
  cb.on_signal_restored = [&] { ++restored; };
  SignalClient cli(TempPath("rec"), MakeSub(), cb, 1000ms, clk.Make());
  cli.Start();
  CHECK(WaitFor([&] { return srv.ConnCount() >= 1; }));

  SignalHeartbeat hb;
  hb.seq = 1;
  hb.ts_us = 1;
  CHECK(srv.SendHeartbeat(hb));
  SignalPush p;
  p.signal = "break-2330";
  p.symbol = "2330";
  p.action = SignalAction::kEnter;
  p.seq = 2;
  p.ts_us = 2;
  CHECK(srv.SendSignal(p));
  CHECK(WaitFor([&] { return signals.load() >= 1; }));  // healthy

  srv.DropCurrent();
  CHECK(WaitFor([&] { return lost.load() >= 1; }));      // socket error -> lost
  CHECK(WaitFor([&] { return srv.ConnCount() >= 2; }));  // auto-reconnect
  CHECK(WaitFor([&] { return srv.Subscribes().size() >= 2; }));
  CHECK(srv.Subscribes()[1] + "\n" == SerializeSubscribe(MakeSub()));  // re-subscribe

  SignalHeartbeat hb2;  // fresh per-connection seq
  hb2.seq = 1;
  hb2.ts_us = 3;
  CHECK(srv.SendHeartbeat(hb2));
  CHECK(WaitFor([&] { return restored.load() >= 1; }));
  CHECK(lost.load() == 1);
  cli.Stop();
}

// (e) seq gap observed across hb + signal frames.
void TestSeqGap() {
  FakeServer srv(TempPath("gap"));
  CHECK(srv.Start());
  FakeClock clk;
  std::atomic<int> gaps{0};
  std::atomic<std::uint64_t> exp{0}, got{0};
  SignalCallbacks cb;
  cb.on_seq_gap = [&](std::uint64_t e, std::uint64_t g) {
    exp = e;
    got = g;
    ++gaps;
  };
  SignalClient cli(TempPath("gap"), MakeSub(), cb, 1000ms, clk.Make());
  cli.Start();
  CHECK(WaitFor([&] { return srv.ConnCount() >= 1; }));

  SignalHeartbeat hb;
  hb.seq = 1;
  hb.ts_us = 1;
  CHECK(srv.SendHeartbeat(hb));
  SignalPush p;
  p.signal = "break-2330";
  p.symbol = "2330";
  p.action = SignalAction::kEnter;
  p.seq = 3;  // skips 2
  p.ts_us = 2;
  CHECK(srv.SendSignal(p));
  CHECK(WaitFor([&] { return gaps.load() >= 1; }));
  CHECK(exp.load() == 2);
  CHECK(got.load() == 3);
  cli.Stop();
}

// (f) Stop() joins cleanly while the server floods signals.
void TestStopUnderFlood() {
  FakeServer srv(TempPath("fld"));
  CHECK(srv.Start());
  FakeClock clk;
  std::atomic<int> signals{0};
  SignalCallbacks cb;
  cb.on_signal = [&](const SignalPush&) { ++signals; };
  SignalClient cli(TempPath("fld"), MakeSub(), cb, 1000ms, clk.Make());
  cli.Start();
  CHECK(WaitFor([&] { return srv.ConnCount() >= 1; }));

  std::atomic<bool> flood{true};
  std::thread flooder([&] {
    SignalPush p;
    p.signal = "break-2330";
    p.symbol = "2330";
    p.action = SignalAction::kEnter;
    p.ts_us = 0;
    std::uint64_t seq = 1;
    while (flood) {
      p.seq = seq++;
      if (!srv.SendSignal(p)) break;
    }
  });
  CHECK(WaitFor([&] { return signals.load() >= 1; }));
  cli.Stop();  // must join without hanging under load
  flood = false;
  flooder.join();
  CHECK(signals.load() >= 1);
}

}  // namespace

int main() {
  TestSubscribeAndSignal();
  TestHeartbeatMissLostOnce();
  TestReconnectRestored();
  TestSeqGap();
  TestStopUnderFlood();
  if (g_failures == 0) {
    std::printf("test_signal_client: OK\n");
    return 0;
  }
  std::printf("test_signal_client: FAILED %d check(s)\n", g_failures);
  return 1;
}
