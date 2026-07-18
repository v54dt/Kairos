// The hub emits a `forwarded` frame to the owning client the instant a queued
// submit clears the forwarder gate (right before it reaches the broker), so the
// trader can rebase its ack-timeout clock and not misread hub queue delay as
// broker silence (Kairos review.md 2026-07-18). These tests prove: forwarded is
// emitted exactly once per forwarded submit, to the owning client only, AFTER the
// halt re-check (a halt-rejected dequeue emits none); withdrawn/purged submits
// never emit; an audit `forwarded` line is written; and, in a same-tick N-client
// burst with a slow broker, the forwarded->ack gap stays under an ack_timeout that
// the naive submit->ack gap blows past (so the restart removes the false timeout).

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "order_codec.h"
#include "order_hub.h"
#include "order_journal.h"
#include "test_check.h"

using namespace kairos::exec;
using namespace std::chrono_literals;

namespace {

// Backend whose Submit parks in a hold gate (test-controlled) and then, once
// released, sleeps a fixed delay before acking — so per-order broker service time
// is deterministic. Acks off the backend lock (OnAck locks the hub).
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
    }
    if (delay_ms_ > 0) std::this_thread::sleep_for(delay_ms_ * 1ms);
    {
      std::lock_guard<std::mutex> lk(mu_);
      submits_.push_back(id);
      cv_.notify_all();
    }
    if (on_ack_) on_ack_(id, true, "");
  }

  void Cancel(const std::string& id) override {
    std::lock_guard<std::mutex> lk(mu_);
    cancels_.push_back(id);
    cv_.notify_all();
  }

  void SetDelayMs(int ms) { delay_ms_ = ms; }
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
  bool WaitEntered(std::size_t n, int ms = 2000) {
    std::unique_lock<std::mutex> lk(mu_);
    return cv_.wait_for(lk, ms * 1ms, [&] { return entered_.size() >= n; });
  }
  bool WaitSubmits(std::size_t n, int ms = 3000) {
    std::unique_lock<std::mutex> lk(mu_);
    return cv_.wait_for(lk, ms * 1ms, [&] { return submits_.size() >= n; });
  }

 private:
  bool connected_ = false;
  bool open_ = true;
  int delay_ms_ = 0;
  std::mutex mu_;
  std::condition_variable gate_;
  std::condition_variable cv_;
  std::vector<std::string> entered_;
  std::vector<std::string> submits_;
  std::vector<std::string> cancels_;
};

// Records every hub->client frame with a monotonic receive timestamp.
struct Sink {
  struct Rec {
    int client;
    OrderMsgKind kind;
    std::string id;
    bool ok;
    std::chrono::steady_clock::time_point t;
  };
  std::mutex mu;
  std::vector<Rec> recs;

  OrderHub::SendFn Fn() {
    return [this](int c, const std::vector<std::uint8_t>& b) {
      OrderMessage m;
      if (!DecodeOrder(b.data(), b.size(), &m)) return;
      Rec r{c, m.kind, "", false, std::chrono::steady_clock::now()};
      if (m.kind == OrderMsgKind::kForwarded) r.id = m.forwarded.id;
      if (m.kind == OrderMsgKind::kAck) {
        r.id = m.ack.id;
        r.ok = m.ack.ok;
      }
      if (m.kind == OrderMsgKind::kCancelResult) {
        r.id = m.cancel_result.id;
        r.ok = m.cancel_result.ok;
      }
      std::lock_guard<std::mutex> lk(mu);
      recs.push_back(std::move(r));
    };
  }
  int CountForwarded(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu);
    int n = 0;
    for (const auto& r : recs)
      if (r.kind == OrderMsgKind::kForwarded && r.id == id) ++n;
    return n;
  }
  int ForwardedClient(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu);
    for (const auto& r : recs)
      if (r.kind == OrderMsgKind::kForwarded && r.id == id) return r.client;
    return -1;
  }
  bool HasAck(const std::string& id, bool ok) {
    std::lock_guard<std::mutex> lk(mu);
    for (const auto& r : recs)
      if (r.kind == OrderMsgKind::kAck && r.id == id && r.ok == ok) return true;
    return false;
  }
  std::chrono::steady_clock::time_point TimeOf(const std::string& id, OrderMsgKind kind) {
    std::lock_guard<std::mutex> lk(mu);
    for (const auto& r : recs)
      if (r.kind == kind && r.id == id) return r.t;
    return {};
  }
};

OrderSubmitMsg Order(const std::string& id) {
  return {id, "2330", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash", "ROD", 10000, 1000};
}
void Submit(OrderHub& hub, int client, const std::string& id) {
  auto b = EncodeOrderSubmit(Order(id));
  hub.OnClientMessage(client, b.data(), b.size());
}
void CancelReq(OrderHub& hub, int client, const std::string& id) {
  auto b = EncodeOrderCancel({id});
  hub.OnClientMessage(client, b.data(), b.size());
}

int LinesOfType(const std::string& path, const std::string& type, const std::string& id) {
  std::ifstream in(path);
  std::string line;
  int n = 0;
  while (std::getline(in, line)) {
    if (line.find("\"type\":\"" + type + "\"") == std::string::npos) continue;
    if (line.find("\"id\":\"" + id + "\"") != std::string::npos) ++n;
  }
  return n;
}

// --- forwarded emitted exactly once, to the owning client, with an audit line ---
void TestForwardedEmittedOnce() {
  const std::string dir = "/tmp/kairos-fwd-once-" + std::to_string(::getpid());
  const std::string fpath = JournalPath(dir, "hub-orders-" + JournalDayUtc8());
  std::remove(fpath.c_str());

  SlowBackend backend;
  Sink sink;
  OrderHub::RiskConfig risk;
  risk.journal_dir = dir;  // order_flow_journal defaults on
  OrderHub hub(&backend, sink.Fn(), risk);
  CHECK(hub.Start());

  Submit(hub, 7, "a");
  CHECK(backend.WaitSubmits(1));
  hub.DrainForwardedForTest();

  CHECK(sink.CountForwarded("a") == 1);              // exactly one forwarded frame
  CHECK(sink.ForwardedClient("a") == 7);             // to the owning client
  CHECK(LinesOfType(fpath, "forwarded", "a") == 1);  // audit line present
  hub.Stop();
}

// --- a halt-rejected dequeue never emits forwarded (nor journals one) ----------
void TestHaltRejectedNoForwarded() {
  const std::string dir = "/tmp/kairos-fwd-halt-" + std::to_string(::getpid());
  const std::string fpath = JournalPath(dir, "hub-orders-" + JournalDayUtc8());
  std::remove(fpath.c_str());

  SlowBackend backend;
  Sink sink;
  OrderHub::RiskConfig risk;
  risk.journal_dir = dir;
  OrderHub hub(&backend, sink.Fn(), risk);
  CHECK(hub.Start());
  backend.Hold();

  Submit(hub, 6, "s0");  // in flight, keeps the forwarder busy
  CHECK(backend.WaitEntered(1));
  Submit(hub, 7, "s1");  // accepted while not halted, queued

  hub.SetAdminHalt(true);  // halt AFTER s1 was accepted
  backend.Open();
  CHECK(backend.WaitSubmits(1));
  hub.DrainForwardedForTest();

  CHECK(sink.CountForwarded("s0") == 1);  // s0 forwarded before the halt
  CHECK(sink.CountForwarded("s1") == 0);  // s1 rejected at dequeue: never forwarded
  CHECK(sink.HasAck("s1", false));        // client told it was rejected
  CHECK(LinesOfType(fpath, "forwarded", "s1") == 0);
  hub.Stop();
}

// --- a locally-withdrawn queued submit never reaches the broker or emits forwarded
void TestWithdrawnNoForwarded() {
  SlowBackend backend;
  Sink sink;
  OrderHub hub(&backend, sink.Fn());
  CHECK(hub.Start());
  backend.Hold();

  Submit(hub, 5, "seed");  // parks the forwarder inside Submit(seed)
  CHECK(backend.WaitEntered(1));
  Submit(hub, 7, "a");     // stays queued behind the parked forwarder
  CancelReq(hub, 7, "a");  // still queued -> withdrawn locally

  backend.Open();
  CHECK(backend.WaitSubmits(1));  // only seed reaches the broker
  hub.DrainForwardedForTest();

  CHECK(sink.CountForwarded("a") == 0);  // never forwarded
  CHECK(sink.CountForwarded("seed") == 1);
  hub.Stop();
}

// --- a disconnect purges the client's queued submit before it can be forwarded ---
void TestPurgedNoForwarded() {
  SlowBackend backend;
  Sink sink;
  OrderHub hub(&backend, sink.Fn());
  CHECK(hub.Start());
  backend.Hold();

  Submit(hub, 6, "s0");  // in flight
  CHECK(backend.WaitEntered(1));
  Submit(hub, 7, "s1");       // queued
  hub.OnClientDisconnect(7);  // purges s1

  backend.Open();
  CHECK(backend.WaitSubmits(1));
  hub.DrainForwardedForTest();

  CHECK(sink.CountForwarded("s1") == 0);  // purged before forwarding
  hub.Stop();
}

// --- burst: N clients submit in the same tick; a slow broker makes the naive
//     submit->ack gap exceed a short ack_timeout, but forwarded->ack stays under it
void TestBurstNoFalseTimeout() {
  constexpr int kN = 6;
  constexpr int kDelayMs = 100;       // per-order broker service time
  constexpr int kAckTimeoutMs = 250;  // shorter than the kN*kDelayMs total drain

  SlowBackend backend;
  Sink sink;
  OrderHub hub(&backend, sink.Fn());
  CHECK(hub.Start());
  backend.SetDelayMs(kDelayMs);
  backend.Hold();  // hold so all kN submits queue in one tick before any drains

  std::unordered_map<std::string, std::chrono::steady_clock::time_point> submit_t;
  for (int c = 0; c < kN; ++c) {
    std::string id = "b" + std::to_string(c);
    submit_t[id] = std::chrono::steady_clock::now();
    Submit(hub, c, id);
  }
  backend.Open();
  CHECK(backend.WaitSubmits(kN));
  hub.DrainForwardedForTest();

  bool adversarial = false;  // at least one order's naive submit->ack blows the budget
  for (int c = 0; c < kN; ++c) {
    std::string id = "b" + std::to_string(c);
    auto fwd = sink.TimeOf(id, OrderMsgKind::kForwarded);
    auto ack = sink.TimeOf(id, OrderMsgKind::kAck);
    CHECK(fwd.time_since_epoch().count() != 0);  // every order was forwarded once
    CHECK(ack.time_since_epoch().count() != 0);  // and acked
    long naive = std::chrono::duration_cast<std::chrono::milliseconds>(ack - submit_t[id]).count();
    long rebased = std::chrono::duration_cast<std::chrono::milliseconds>(ack - fwd).count();
    if (naive > kAckTimeoutMs) adversarial = true;
    // With the forwarded restart the clock is measured from broker-forward, not
    // hub-accept, so no order's window is blown by queue delay.
    CHECK(rebased < kAckTimeoutMs);
  }
  CHECK(adversarial);  // the setup really would false-timeout without the restart
  hub.Stop();
}

}  // namespace

int main() {
  TestForwardedEmittedOnce();
  TestHaltRejectedNoForwarded();
  TestWithdrawnNoForwarded();
  TestPurgedNoForwarded();
  TestBurstNoFalseTimeout();

  if (g_failures == 0) {
    std::printf("test_order_hub_forwarded: OK\n");
    return 0;
  }
  std::printf("test_order_hub_forwarded: FAILED %d check(s)\n", g_failures);
  return 1;
}
