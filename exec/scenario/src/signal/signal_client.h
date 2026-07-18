#ifndef KAIROS_EXEC_SIGNAL_CLIENT_H_
#define KAIROS_EXEC_SIGNAL_CLIENT_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "signal_proto.h"
#include "socket_path.h"

namespace kairos::exec {

// Signal UDS (daemon <-> traders). Never /tmp; see socket_path.h.
inline std::string SignalSocketPath() {
  std::string p = ResolveSock(std::getenv("KAIROS_SIGNAL_SOCK"), std::getenv("XDG_RUNTIME_DIR"),
                              RunUserDir(), "kairos-signals.sock");
  if (p.empty()) NoRuntimeDir("KAIROS_SIGNAL_SOCK");
  return p;
}

// Monotonic clock, injected so tests drive heartbeat liveness deterministically
// (EngineClock style). Only mono time gates the miss decision.
struct SignalClock {
  std::function<std::chrono::steady_clock::time_point()> mono = [] {
    return std::chrono::steady_clock::now();
  };
};

struct SignalCallbacks {
  std::function<void(const SignalPush&)> on_signal;
  std::function<void(const std::string& reason)> on_signal_lost;
  std::function<void()> on_signal_restored;
  std::function<void(std::uint64_t expected, std::uint64_t got)> on_seq_gap;
};

// Connects to the signal UDS, subscribes, and dispatches decoded signals. The
// source is HEALTHY only while the socket is connected and heartbeats keep
// arriving; three missed heartbeat intervals or any socket error fires
// on_signal_lost exactly once, a reconnect plus the first heartbeat fires
// on_signal_restored (and re-subscribes). Reconnects with capped backoff; the
// backoff resets only once a heartbeat proves the link, so a peer that accepts
// then drops keeps escalating, and a run of never-healthy attempts fires
// on_signal_lost ("connect flapping") once instead of spinning silently.
class SignalClient {
 public:
  SignalClient(std::string socket_path, SignalSubscribe sub, SignalCallbacks cb,
               std::chrono::milliseconds hb_interval = std::chrono::milliseconds(1000),
               SignalClock clock = {});
  ~SignalClient();

  SignalClient(const SignalClient&) = delete;
  SignalClient& operator=(const SignalClient&) = delete;

  void Start();
  void Stop();

 private:
  enum class Health { kInitial, kHealthy, kLost };

  int ConnectAndSubscribe();
  void Run();
  void ReadLoop(int fd);
  void HandleLine(const std::string& line);
  void HandleSeq(std::uint64_t seq);
  void OnHeartbeat();
  void MarkFlapping();  // never-healthy connect crash-loop -> lost, exactly once
  void MarkLostFromError();
  bool CheckLiveness();  // true => tear the connection down and reconnect

  std::string socket_path_;
  SignalSubscribe sub_;
  SignalCallbacks cb_;
  std::chrono::milliseconds hb_interval_;
  SignalClock clock_;

  std::atomic<bool> stop_{false};
  std::atomic<int> fd_{-1};
  std::thread thread_;

  std::mutex mu_;
  Health health_ = Health::kInitial;
  bool proven_healthy_ = false;  // a heartbeat arrived on the current connection
  std::chrono::steady_clock::time_point last_hb_;
  std::uint64_t last_seq_ = 0;
  bool have_seq_ = false;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SIGNAL_CLIENT_H_
