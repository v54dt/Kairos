// The order hub forwards broker submits/cancels on ONE background thread so a
// client reader thread never blocks in the broker's shared rate gate. These
// tests drive a slow/holdable fake backend (no sockets) to prove: a submit
// returns before the backend sees it, per-client FIFO is preserved under a
// concurrent burst, a cancel of a still-queued submit never reaches the broker
// (and frees its reservation), cancels drain before queued submits, a client
// disconnect purges its queued submits, admin halt is re-checked at dequeue,
// shutdown fails closed (queued-but-unforwarded submits are never sent), and a
// broker-bound cancel still queued at shutdown is forwarded, not dropped.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hub_status.h"
#include "order_codec.h"
#include "order_hub.h"
#include "test_check.h"

using namespace kairos::exec;
using namespace std::chrono_literals;

namespace {

// Backend whose Submit blocks while "held", so the test controls exactly when an
// order reaches the broker. Records every submit/cancel in one ordered event log
// under a mutex; every accessor is thread-safe (the forwarder writes, the test
// reads). Submit acks immediately once released, so acked routes exist.
class SlowBackend : public OrderBackend {
 public:
  bool Connect() override {
    connected_ = true;
    return true;
  }
  void Disconnect() override { connected_ = false; }
  bool IsConnected() const override { return connected_; }

  void Submit(const OrderSubmitMsg& o) override {
    std::string id = o.id;
    {
      std::unique_lock<std::mutex> lk(mu_);
      entered_.push_back(id);
      cv_.notify_all();
      gate_.wait(lk, [this] { return open_; });
      submits_.push_back(id);
      events_.push_back("S:" + id);
      cv_.notify_all();
    }
    if (on_ack_) on_ack_(id, true, "");  // off the backend lock: OnAck locks the hub
  }

  void Cancel(const std::string& id) override {
    std::lock_guard<std::mutex> lk(mu_);
    cancels_.push_back(id);
    events_.push_back("C:" + id);
    cv_.notify_all();
  }

  void Hold() {
    std::lock_guard<std::mutex> lk(mu_);
    open_ = false;
  }
  void Open() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      open_ = true;
    }
    gate_.notify_all();
  }

  std::size_t NumSubmits() {
    std::lock_guard<std::mutex> lk(mu_);
    return submits_.size();
  }
  std::vector<std::string> Submits() {
    std::lock_guard<std::mutex> lk(mu_);
    return submits_;
  }
  std::vector<std::string> Cancels() {
    std::lock_guard<std::mutex> lk(mu_);
    return cancels_;
  }
  std::vector<std::string> Events() {
    std::lock_guard<std::mutex> lk(mu_);
    return events_;
  }
  bool WaitEntered(std::size_t n, int ms = 2000) {
    std::unique_lock<std::mutex> lk(mu_);
    return cv_.wait_for(lk, ms * 1ms, [&] { return entered_.size() >= n; });
  }
  bool WaitSubmits(std::size_t n, int ms = 2000) {
    std::unique_lock<std::mutex> lk(mu_);
    return cv_.wait_for(lk, ms * 1ms, [&] { return submits_.size() >= n; });
  }
  bool WaitCancels(std::size_t n, int ms = 2000) {
    std::unique_lock<std::mutex> lk(mu_);
    return cv_.wait_for(lk, ms * 1ms, [&] { return cancels_.size() >= n; });
  }

 private:
  bool connected_ = false;
  bool open_ = true;  // Hold() to make Submit block
  std::mutex mu_;
  std::condition_variable gate_;  // Submit parks here while !open_
  std::condition_variable cv_;    // signals entered/submit/cancel arrivals
  std::vector<std::string> entered_;
  std::vector<std::string> submits_;
  std::vector<std::string> cancels_;
  std::vector<std::string> events_;
};

// Thread-safe recorder of hub->client frames (send_ runs on both the client and
// the forwarder threads).
struct Sink {
  std::mutex mu;
  std::vector<std::pair<int, OrderMessage>> sent;
  OrderHub::SendFn Fn() {
    return [this](int c, const std::vector<std::uint8_t>& b) {
      OrderMessage m;
      if (!DecodeOrder(b.data(), b.size(), &m)) return;
      std::lock_guard<std::mutex> lk(mu);
      sent.push_back({c, m});
    };
  }
  bool Has(const std::string& id, OrderMsgKind kind, bool ok) {
    std::lock_guard<std::mutex> lk(mu);
    for (const auto& [c, m] : sent) {
      (void)c;
      if (m.kind != kind) continue;
      if (kind == OrderMsgKind::kAck && m.ack.id == id && m.ack.ok == ok) return true;
      if (kind == OrderMsgKind::kCancelResult && m.cancel_result.id == id &&
          m.cancel_result.ok == ok)
        return true;
    }
    return false;
  }
};

OrderSubmitMsg Order(const std::string& id, Side side, Cents price, long shares) {
  return {id, "2330", Market::kTse, Board::kRoundLot, side, "Cash", "ROD", price, shares};
}

void Submit(OrderHub& hub, int client, const OrderSubmitMsg& o) {
  auto b = EncodeOrderSubmit(o);
  hub.OnClientMessage(client, b.data(), b.size());
}
void CancelReq(OrderHub& hub, int client, const std::string& id) {
  auto b = EncodeOrderCancel({id});
  hub.OnClientMessage(client, b.data(), b.size());
}

// --- a submit returns before the backend is ever called ------------------
void TestSubmitReturnsBeforeBackend() {
  SlowBackend backend;
  Sink sink;
  OrderHub hub(&backend, sink.Fn());
  CHECK(hub.Start());
  backend.Hold();

  Submit(hub, 7, Order("a", Side::kBuy, 10000, 1000));  // enqueues, returns at once
  CHECK(backend.NumSubmits() == 0);                     // backend held: forward has not happened

  backend.Open();
  CHECK(backend.WaitSubmits(1));
  CHECK(backend.Submits() == std::vector<std::string>{"a"});
  hub.Stop();
}

// --- 10 clients submit concurrently; each client's order is preserved -----
void TestFifoBurst() {
  SlowBackend backend;
  Sink sink;
  OrderHub hub(&backend, sink.Fn());
  CHECK(hub.Start());
  backend.Hold();  // hold so every submit is queued before any is forwarded

  constexpr int kClients = 10;
  constexpr int kPer = 10;
  std::vector<std::thread> ts;
  for (int c = 0; c < kClients; ++c) {
    ts.emplace_back([&, c] {
      for (int i = 0; i < kPer; ++i)
        Submit(hub, c,
               Order("c" + std::to_string(c) + "-" + std::to_string(i), Side::kBuy, 10000, 1000));
    });
  }
  for (auto& t : ts) t.join();

  backend.Open();
  CHECK(backend.WaitSubmits(kClients * kPer));
  auto got = backend.Submits();
  CHECK(got.size() == static_cast<std::size_t>(kClients * kPer));

  // Per-client FIFO: each client's ids appear in submitted order, no gaps/dupes.
  std::vector<int> next(kClients, 0);
  for (const auto& id : got) {
    int c = 0, i = 0;
    std::sscanf(id.c_str(), "c%d-%d", &c, &i);
    CHECK(c >= 0 && c < kClients);
    CHECK(i == next[c]);  // this client's next expected seq
    ++next[c];
  }
  for (int c = 0; c < kClients; ++c) CHECK(next[c] == kPer);
  hub.Stop();
}

// --- cancel of a still-queued submit: broker never sees it, reservation freed
void TestCancelOfQueued() {
  SlowBackend backend;
  Sink sink;
  OrderHub::RiskConfig cfg;
  cfg.max_open_orders_per_client = 1;  // one outstanding intent per client
  OrderHub hub(&backend, sink.Fn(), cfg);
  CHECK(hub.Start());
  backend.Hold();

  // Park the forwarder inside Submit(seed) so the next submits stay queued.
  Submit(hub, 5, Order("seed", Side::kBuy, 10000, 1000));
  CHECK(backend.WaitEntered(1));

  Submit(hub, 7, Order("a", Side::kBuy, 10000, 1000));  // queued; reserves client 7's one slot
  Submit(hub, 7, Order("b", Side::kBuy, 10000, 1000));  // over the per-client limit -> rejected
  CHECK(sink.Has("b", OrderMsgKind::kAck, false));      // reservation counted while "a" was queued

  CancelReq(hub, 7, "a");                                   // still queued -> withdraw locally
  CHECK(sink.Has("a", OrderMsgKind::kCancelResult, true));  // instant ok, before any broker call

  Submit(hub, 7, Order("c", Side::kBuy, 10000, 1000));  // slot freed by the withdraw -> admitted

  backend.Open();
  CHECK(backend.WaitSubmits(2));  // seed + c
  auto got = backend.Submits();
  CHECK(got == (std::vector<std::string>{"seed", "c"}));  // "a" never reached the broker
  CHECK(backend.Cancels().empty());                       // withdraw issues no broker cancel
  hub.Stop();
}

// --- cancels drain before queued submits ---------------------------------
void TestCancelPriority() {
  SlowBackend backend;
  Sink sink;
  OrderHub hub(&backend, sink.Fn());
  CHECK(hub.Start());
  backend.Hold();

  Submit(hub, 7, Order("s1", Side::kBuy, 10000, 1000));
  CHECK(backend.WaitEntered(1));  // forwarder parked in Submit(s1)

  Submit(hub, 7, Order("s2", Side::kBuy, 10000, 1000));
  Submit(hub, 7, Order("s3", Side::kBuy, 10000, 1000));
  CancelReq(hub, 7, "gone");  // unknown id -> a broker cancel is queued

  backend.Open();
  CHECK(backend.WaitSubmits(3));
  CHECK(backend.WaitCancels(1));
  // After s1 (in flight), the queued cancel jumps ahead of s2/s3.
  CHECK(backend.Events() == (std::vector<std::string>{"S:s1", "C:gone", "S:s2", "S:s3"}));
  hub.Stop();
}

// --- a disconnect purges the client's still-queued submits ----------------
void TestDisconnectPurge() {
  SlowBackend backend;
  Sink sink;
  OrderHub hub(&backend, sink.Fn());
  CHECK(hub.Start());
  backend.Hold();

  Submit(hub, 6, Order("s0", Side::kBuy, 10000, 1000));  // client 6, kept in flight
  CHECK(backend.WaitEntered(1));
  Submit(hub, 7, Order("s1", Side::kBuy, 10000, 1000));  // client 7, stays queued

  CHECK(hub.CaptureStatus().account_open_notional_cents == 20000000);  // both reserved
  hub.OnClientDisconnect(7);                                           // purges s1
  CHECK(hub.CaptureStatus().account_open_notional_cents == 10000000);  // s1 reservation freed

  backend.Open();
  CHECK(backend.WaitSubmits(1));
  CHECK(backend.Submits() == std::vector<std::string>{"s0"});  // s1 never forwarded
  hub.Stop();
}

// --- admin halt set after accept is re-checked at dequeue -----------------
void TestHaltAtDequeue() {
  SlowBackend backend;
  Sink sink;
  OrderHub hub(&backend, sink.Fn());
  CHECK(hub.Start());
  backend.Hold();

  Submit(hub, 6, Order("s0", Side::kBuy, 10000, 1000));  // in flight, keeps the forwarder busy
  CHECK(backend.WaitEntered(1));
  Submit(hub, 7, Order("s1", Side::kBuy, 10000, 1000));  // accepted while not halted, queued

  hub.SetAdminHalt(true);  // halt AFTER s1 was accepted
  backend.Open();
  CHECK(backend.WaitSubmits(1));
  hub.DrainForwardedForTest();  // let the forwarder reach and reject s1 at dequeue

  CHECK(backend.Submits() == std::vector<std::string>{"s0"});  // s1 rejected, never sent
  CHECK(sink.Has("s1", OrderMsgKind::kAck, false));            // client told it was rejected
  CHECK(hub.CaptureStatus().account_open_notional_cents == 10000000);  // s1 reservation freed
  hub.Stop();
}

// --- shutdown fails closed: queued-but-unforwarded submits are not sent ----
void TestShutdownFailClosed() {
  SlowBackend backend;
  Sink sink;
  OrderHub hub(&backend, sink.Fn());
  CHECK(hub.Start());
  backend.Hold();

  Submit(hub, 7, Order("s1", Side::kBuy, 10000, 1000));  // popped, parks in Submit(s1)
  CHECK(backend.WaitEntered(1));
  Submit(hub, 7, Order("s2", Side::kBuy, 10000, 1000));  // queued, never popped

  // Release the in-flight s1 only after Stop() has set the stop flag, so the
  // forwarder observes stop before it could pop s2.
  std::thread opener([&] {
    std::this_thread::sleep_for(80ms);
    backend.Open();
  });
  hub.Stop();  // sets stop, then joins the forwarder (which fails s2 closed)
  opener.join();

  CHECK(backend.Submits() == std::vector<std::string>{"s1"});  // s2 never forwarded
  CHECK(sink.Has("s2", OrderMsgKind::kAck, false));            // s2 rejected on shutdown
}

// --- shutdown forwards a queued broker cancel instead of dropping it -------
void TestShutdownForwardsCancel() {
  SlowBackend backend;
  Sink sink;
  OrderHub hub(&backend, sink.Fn());
  CHECK(hub.Start());
  backend.Hold();

  Submit(hub, 7, Order("s1", Side::kBuy, 10000, 1000));  // popped, parks in Submit(s1)
  CHECK(backend.WaitEntered(1));
  CancelReq(hub, 7, "live");  // unknown id -> a broker cancel is queued, still un-drained

  // Release the in-flight s1 only after Stop() set the stop flag, so the forwarder
  // observes stop with the cancel still queued and drains it on the way out.
  std::thread opener([&] {
    std::this_thread::sleep_for(80ms);
    backend.Open();
  });
  hub.Stop();  // sets stop, joins the forwarder (which must forward the queued cancel)
  opener.join();

  CHECK(backend.Cancels() == std::vector<std::string>{"live"});  // cancel reached the broker
}

// --- stress: 10 clients x many submits, bounded shutdown, no loss/dup ------
void TestStress() {
  SlowBackend backend;
  Sink sink;
  OrderHub hub(&backend, sink.Fn());
  CHECK(hub.Start());  // backend open: forwards flow while clients pound it

  constexpr int kClients = 10;
  constexpr int kPer = 20;
  std::atomic<int> submitted{0};
  std::vector<std::thread> ts;
  for (int c = 0; c < kClients; ++c) {
    ts.emplace_back([&, c] {
      for (int i = 0; i < kPer; ++i) {
        Submit(hub, c,
               Order("s" + std::to_string(c) + "-" + std::to_string(i), Side::kBuy, 10000, 1000));
        ++submitted;
      }
    });
  }
  for (auto& t : ts) t.join();
  CHECK(submitted.load() == kClients * kPer);
  CHECK(backend.WaitSubmits(kClients * kPer, 5000));

  auto got = backend.Submits();
  CHECK(got.size() == static_cast<std::size_t>(kClients * kPer));
  std::vector<int> next(kClients, 0);
  for (const auto& id : got) {
    int c = 0, i = 0;
    std::sscanf(id.c_str(), "s%d-%d", &c, &i);
    CHECK(i == next[c]);  // per-client order preserved, no loss or duplication
    ++next[c];
  }
  hub.Stop();  // bounded: the forwarder is idle or in one in-flight call
}

}  // namespace

int main() {
  TestSubmitReturnsBeforeBackend();
  TestFifoBurst();
  TestCancelOfQueued();
  TestCancelPriority();
  TestDisconnectPurge();
  TestHaltAtDequeue();
  TestShutdownFailClosed();
  TestShutdownForwardsCancel();
  TestStress();

  if (g_failures == 0) {
    std::printf("test_order_hub_forwarder: OK\n");
    return 0;
  }
  std::printf("test_order_hub_forwarder: FAILED %d check(s)\n", g_failures);
  return 1;
}
