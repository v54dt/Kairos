// Signal daemon integration test: drives the REAL PR1 SignalClient (and a raw fd
// client) against a live daemon over UDS in the runtime dir (never /tmp). Proves
// subscribe -> sub_ack ok, heartbeat + signal share one strictly monotonic
// per-connection seq (no on_seq_gap), a manual spool line emits exactly once per
// append, an unknown predicate/symbol yields sub_ack ok=false, and a malformed
// line leaves the connection alive. Real fast heartbeat keeps the client healthy;
// WaitFor makes the assertions deterministic.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "signal_client.h"
#include "signal_daemon.h"
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

std::string RuntimeDir() {
  if (const char* x = std::getenv("XDG_RUNTIME_DIR"); x != nullptr && x[0] != '\0') return x;
  return "/run/user/" + std::to_string(::getuid());
}

std::string PathIn(const char* tag) {
  return RuntimeDir() + "/ktsd-" + std::to_string(::getpid()) + "-" + tag;
}

void WriteFile(const std::string& path, const std::string& content) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  CHECK(f != nullptr);
  if (f == nullptr) return;
  std::fwrite(content.data(), 1, content.size(), f);
  std::fclose(f);
}

void AppendLine(const std::string& path, const std::string& line) {
  std::FILE* f = std::fopen(path.c_str(), "ab");
  CHECK(f != nullptr);
  if (f == nullptr) return;
  std::fwrite(line.data(), 1, line.size(), f);
  std::fclose(f);
}

// Blocking raw UDS client that can read whole frames with a deadline.
class RawClient {
 public:
  bool Connect(const std::string& path) {
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    timeval tv{0, 100000};  // 100ms recv tick
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  }
  ~RawClient() {
    if (fd_ >= 0) ::close(fd_);
  }

  void SendRaw(const std::string& s) { ::send(fd_, s.data(), s.size(), MSG_NOSIGNAL); }
  void Subscribe(const std::string& sig, const std::string& sym) {
    SendRaw(SerializeSubscribe(SignalSubscribe{sig, sym}));
  }

  // Read frames until one of ServerFrameType `want` arrives (default: sub_ack).
  bool NextAck(SignalAck* out, std::chrono::milliseconds timeout = 3s) {
    auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
      std::string line;
      if (!NextLine(&line, end)) continue;
      ServerFrame f;
      std::string err;
      if (ParseServerFrame(line, &f, &err) && f.type == ServerFrameType::kSubAck) {
        *out = f.ack;
        return true;
      }
    }
    return false;
  }

 private:
  bool NextLine(std::string* out, std::chrono::steady_clock::time_point end) {
    while (buf_.find('\n') == std::string::npos) {
      if (std::chrono::steady_clock::now() >= end) return false;
      char chunk[512];
      ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
      if (n > 0) buf_.append(chunk, static_cast<std::size_t>(n));
    }
    std::size_t nl = buf_.find('\n');
    *out = buf_.substr(0, nl);
    buf_.erase(0, nl + 1);
    return true;
  }

  int fd_ = -1;
  std::string buf_;
};

const char* kConfig =
    "[[predicate]]\n"
    "kind = \"manual\"\n"
    "name = \"manual\"\n"
    "symbols = [\"2330\", \"2317\"]\n"
    "\n"
    "[[predicate]]\n"
    "kind = \"depth_evap\"\n"
    "name = \"depth-2330\"\n"
    "symbols = [\"2330\"]\n"
    "side = \"bid\"\n"
    "window_s = 10.0\n"
    "ratio = 0.4\n";

void RunAll() {
  const std::string sock = PathIn("d.sock");
  const std::string spool = PathIn("spool");
  const std::string config = PathIn("toml");
  const std::string dead_quote = PathIn("noquote.sock");  // no listener; harmless
  WriteFile(config, kConfig);
  WriteFile(spool, "");
  ::unlink(sock.c_str());

  SignalRegistry registry = BuildSignalRegistry(config, spool);
  SignalDaemon::Options opts;
  opts.signal_sock = sock;
  opts.quote_sock = dead_quote;
  opts.hb_interval = 50ms;  // fast heartbeat keeps the client healthy
  opts.poll_interval = 20ms;
  SignalDaemon daemon(std::move(registry), std::move(opts));
  CHECK(daemon.Start());

  // (a) real PR1 client: subscribe, stay healthy, observe seq + a manual signal.
  std::atomic<int> signals{0}, gaps{0}, lost{0};
  std::string got_action;
  std::mutex got_mu;
  SignalCallbacks cb;
  cb.on_signal = [&](const SignalPush& p) {
    {
      std::lock_guard<std::mutex> l(got_mu);
      got_action = SignalActionName(p.action);
    }
    ++signals;
  };
  cb.on_seq_gap = [&](std::uint64_t, std::uint64_t) { ++gaps; };
  cb.on_signal_lost = [&](const std::string&) { ++lost; };
  SignalClient cli(sock, SignalSubscribe{"manual", "2330"}, cb);
  cli.Start();
  CHECK(WaitFor([&] { return daemon.SubscriberCount("manual", "2330") >= 1; }));

  AppendLine(spool, "{\"signal\":\"manual\",\"symbol\":\"2330\",\"action\":\"enter\"}\n");
  CHECK(WaitFor([&] { return signals.load() >= 1; }));
  {
    std::lock_guard<std::mutex> l(got_mu);
    CHECK(got_action == "enter");
  }
  std::this_thread::sleep_for(120ms);  // no second delivery for one appended line
  CHECK(signals.load() == 1);

  AppendLine(spool, "{\"signal\":\"manual\",\"symbol\":\"2330\",\"action\":\"exit\"}\n");
  CHECK(WaitFor([&] { return signals.load() >= 2; }));
  CHECK(gaps.load() == 0);  // hb + signal share one monotonic per-conn seq
  CHECK(lost.load() == 0);  // fast heartbeat kept it healthy throughout

  // (b) sub_ack: known ok, unknown predicate + unknown symbol -> ok=false.
  RawClient ok_client;
  CHECK(ok_client.Connect(sock));
  ok_client.Subscribe("manual", "2317");
  SignalAck ack;
  CHECK(ok_client.NextAck(&ack));
  CHECK(ack.ok);

  RawClient bad_pred;
  CHECK(bad_pred.Connect(sock));
  bad_pred.Subscribe("nope", "2330");
  CHECK(bad_pred.NextAck(&ack));
  CHECK(!ack.ok);

  RawClient bad_sym;
  CHECK(bad_sym.Connect(sock));
  bad_sym.Subscribe("manual", "9999");
  CHECK(bad_sym.NextAck(&ack));
  CHECK(!ack.ok);

  // (c) malformed line -> ok=false, connection survives -> a later valid sub acks.
  RawClient survivor;
  CHECK(survivor.Connect(sock));
  survivor.SendRaw("{ not json at all\n");
  CHECK(survivor.NextAck(&ack));
  CHECK(!ack.ok);
  survivor.Subscribe("manual", "2330");
  CHECK(survivor.NextAck(&ack));
  CHECK(ack.ok);  // same connection still works after the malformed line

  cli.Stop();
  daemon.Stop();
  ::unlink(config.c_str());
  ::unlink(spool.c_str());
}

}  // namespace

int main() {
  RunAll();
  if (g_failures == 0) {
    std::printf("test_signal_daemon: OK\n");
    return 0;
  }
  std::printf("test_signal_daemon: FAILED %d check(s)\n", g_failures);
  return 1;
}
