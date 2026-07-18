#ifndef KAIROS_EXEC_SIGNAL_DAEMON_H_
#define KAIROS_EXEC_SIGNAL_DAEMON_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "predicate.h"
#include "quote_book.h"
#include "quote_source.h"
#include "signal_proto.h"

namespace kairos::exec {

// One predicate firing tagged with its predicate name; the (signal, symbol) pair
// is the subscription key the daemon dispatches on.
struct SignalEmit {
  std::string signal;
  std::string symbol;
  PredicateFire fire;
};

// Owns the configured predicates and turns feed/poll events into emits. Pure: it
// holds no locks and does no I/O, so the daemon serializes it under one mutex and
// tests drive it on synthetic sequences.
class SignalRegistry {
 public:
  void Add(std::unique_ptr<Predicate> predicate);

  // Union of the symbols of the quote-driven predicates (the UDS subscription).
  std::vector<std::string> QuoteSymbols() const;

  // True if (signal, symbol) names a configured predicate and one of its symbols.
  bool Validate(const std::string& signal, const std::string& symbol) const;

  std::vector<SignalEmit> OnQuote(const std::string& symbol, const TopOfBook& tob,
                                  std::int64_t ts_us);
  std::vector<SignalEmit> OnTrade(const std::string& symbol, const Trade& trade,
                                  std::int64_t ts_us);
  std::vector<SignalEmit> Poll(std::int64_t ts_us);

 private:
  std::vector<std::unique_ptr<Predicate>> predicates_;
};

// Builds a registry from a signald TOML file (throws std::runtime_error on any
// missing/invalid key). `spool_path` is handed to every manual predicate.
SignalRegistry BuildSignalRegistry(const std::string& toml_path, const std::string& spool_path);

// Injected clock (EngineClock style): mono paces heartbeats and stamps predicate
// time; wall stamps the informational ts_us of outbound frames. Defaults to the
// real system clocks.
struct SignalDaemonClock {
  std::function<std::chrono::steady_clock::time_point()> mono = [] {
    return std::chrono::steady_clock::now();
  };
  std::function<std::int64_t()> wall_us = [] {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  };
  std::int64_t MonoUs() const {
    return std::chrono::duration_cast<std::chrono::microseconds>(mono().time_since_epoch()).count();
  }
};

// Server side of the signal protocol. Subscribes to the core quote UDS for the
// union of predicate symbols, evaluates predicates on each quote/trade/poll, and
// pushes signals plus a 1s heartbeat to subscribed clients over its own UDS. Each
// connection owns a monotonic seq shared by heartbeats and signals.
class SignalDaemon {
 public:
  struct Options {
    std::string signal_sock;
    std::string quote_sock;
    std::chrono::milliseconds hb_interval{1000};
    std::chrono::milliseconds poll_interval{50};
  };

  SignalDaemon(SignalRegistry registry, Options options, SignalDaemonClock clock = {},
               std::unique_ptr<QuoteSource> quote_source = nullptr);
  ~SignalDaemon();

  SignalDaemon(const SignalDaemon&) = delete;
  SignalDaemon& operator=(const SignalDaemon&) = delete;

  bool Start();
  void Stop();

  // Live subscriber count for (signal, symbol). Lets a test await registration
  // before it drives an event; otherwise unused.
  std::size_t SubscriberCount(const std::string& signal, const std::string& symbol);

 private:
  // Each connection has its own writer thread draining a bounded outbound queue,
  // so a slow/wedged client is dropped rather than head-of-line-blocking the
  // shared heartbeat/dispatch threads writing to every other client.
  struct Conn {
    int fd = -1;
    std::set<std::pair<std::string, std::string>> subs;  // guarded by clients_mu_

    std::mutex write_mu;  // guards q, q_bytes, seq, closed
    std::condition_variable write_cv;
    std::deque<std::string> q;  // serialized frames pending write
    std::size_t q_bytes = 0;
    std::uint64_t seq = 0;  // shared by heartbeats + signals
    bool closed = false;    // writer should drain-and-exit; client is being dropped
    std::thread writer;
  };

  void AcceptLoop();
  void ClientLoop(std::shared_ptr<Conn> conn);
  void WriterLoop(const std::shared_ptr<Conn>& conn);
  void HeartbeatLoop();
  void PollLoop();

  void HandleSubscribe(const std::shared_ptr<Conn>& conn, const std::string& line);
  void Dispatch(const std::vector<SignalEmit>& emits);
  void EnqueueLocked(Conn& conn, std::string frame);
  void SendHeartbeat(Conn& conn);
  void SendSignal(Conn& conn, const SignalEmit& emit);
  void SendAck(Conn& conn, const SignalAck& ack);
  std::vector<std::shared_ptr<Conn>> ConnSnapshot();

  SignalRegistry registry_;
  Options opts_;
  SignalDaemonClock clock_;
  std::unique_ptr<QuoteSource> quote_source_;

  std::atomic<int> listen_fd_{-1};
  std::atomic<bool> stop_{false};
  std::atomic<int> active_clients_{0};
  std::thread accept_thread_;
  std::thread hb_thread_;
  std::thread poll_thread_;

  std::mutex eval_mu_;  // serializes SignalRegistry (predicate state)
  std::mutex clients_mu_;
  std::unordered_map<int, std::shared_ptr<Conn>> conns_;
  std::map<std::pair<std::string, std::string>, std::set<std::shared_ptr<Conn>>> subscribers_;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SIGNAL_DAEMON_H_
