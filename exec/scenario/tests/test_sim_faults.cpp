// Unit coverage for the sim hub's seeded fault injector, driven directly against
// SimOrderBackend via capturing SetCallbacks (no sockets): each knob does what it
// says, the same seed replays the same sequence, and an all-off config is
// byte-identical to today's SimOrderBackend.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fault_config.h"
#include "fault_injector.h"
#include "quote_book.h"
#include "sim_order_backend.h"
#include "test_check.h"

using namespace kairos::exec;

namespace {

std::int64_t Us(int hh, int mm, int ss = 0) {
  long local = hh * 3600 + mm * 60 + ss;
  return static_cast<std::int64_t>(local - 8 * 3600) * 1000000;
}

TopOfBook Book(std::vector<Level> bids, std::vector<Level> asks, std::int64_t ts) {
  TopOfBook t;
  for (auto& b : bids) {
    if (t.n_bids >= TopOfBook::kMaxLevels) break;
    t.bids[t.n_bids++] = b;
  }
  for (auto& a : asks) {
    if (t.n_asks >= TopOfBook::kMaxLevels) break;
    t.asks[t.n_asks++] = a;
  }
  t.quote_ts_us = ts;
  t.valid = true;
  return t;
}

OrderSubmitMsg Order(const std::string& id, Side side, Cents price, long shares) {
  return {id, "2330", Market::kTse, Board::kRoundLot, side, "Cash", "ROD", price, shares};
}

// Records every callback the backend routes, thread-safely (delayed acks arrive on
// the injector's worker thread).
struct Recorder {
  std::mutex mu;
  std::vector<std::pair<std::string, bool>> acks;  // (id, ok)
  std::vector<std::string> ack_errors;
  std::vector<Fill> fills;

  void Wire(SimOrderBackend& b) {
    b.SetCallbacks(
        [this](const std::string& id, bool ok, const std::string& e) {
          std::lock_guard<std::mutex> l(mu);
          acks.push_back({id, ok});
          ack_errors.push_back(e);
        },
        [this](const std::string&, const Fill& f) {
          std::lock_guard<std::mutex> l(mu);
          fills.push_back(f);
        },
        [](const std::string&, bool) {});
  }
  std::size_t AckCount() {
    std::lock_guard<std::mutex> l(mu);
    return acks.size();
  }
  std::size_t FillCount() {
    std::lock_guard<std::mutex> l(mu);
    return fills.size();
  }
};

// Drive a continuous crossing buy so the conservative model fills it fully.
void Trade(SimOrderBackend& b) {
  b.OnTrade("2330", ::kairos::exec::Trade{9900, 1000, Us(10, 0, 5), false});
}
void OpenContinuous(SimOrderBackend& b) {
  b.OnBook("2330", Book({{9990, 100}}, {{10100, 100}}, Us(10, 0, 0)));
}

void TestAllOffIdentical() {
  // A default FaultConfig must produce the exact ack+fill the un-faulted backend
  // does, in the same order.
  Recorder ra, rb;
  SimOrderBackend base(FillMode::kConservative, {"2330"});
  SimOrderBackend off(FillMode::kConservative, {"2330"}, FaultConfig{});
  ra.Wire(base);
  rb.Wire(off);
  OpenContinuous(base);
  OpenContinuous(off);
  base.Submit(Order("k-1", Side::kBuy, 10000, 1000));
  off.Submit(Order("k-1", Side::kBuy, 10000, 1000));
  Trade(base);
  Trade(off);
  CHECK(ra.acks.size() == 1 && rb.acks.size() == 1);
  CHECK(ra.acks[0].second == rb.acks[0].second && rb.acks[0].second);
  CHECK(ra.fills.size() == 1 && rb.fills.size() == 1);
  CHECK(ra.fills[0].shares == rb.fills[0].shares && ra.fills[0].price == rb.fills[0].price);
}

void TestReject() {
  FaultConfig cfg;
  cfg.seed = 1;
  cfg.reject_rate = 1.0;
  Recorder r;
  SimOrderBackend b(FillMode::kConservative, {"2330"}, cfg);
  r.Wire(b);
  OpenContinuous(b);
  b.Submit(Order("k-1", Side::kBuy, 10000, 1000));
  Trade(b);  // no engine order exists, so no fill can ever happen
  CHECK(r.acks.size() == 1);
  CHECK(!r.acks.empty() && !r.acks[0].second);  // rejected ack
  CHECK(!r.ack_errors.empty() && !r.ack_errors[0].empty());
  CHECK(r.fills.empty());
}

void TestDropAck() {
  FaultConfig cfg;
  cfg.seed = 7;
  cfg.ack_drop_rate = 1.0;
  Recorder r;
  SimOrderBackend b(FillMode::kConservative, {"2330"}, cfg);
  r.Wire(b);
  OpenContinuous(b);
  b.Submit(Order("k-1", Side::kBuy, 10000, 1000));
  Trade(b);
  // ack never delivered, but the order still lives and fills (broker accepted, ack lost).
  CHECK(r.AckCount() == 0);
  CHECK(r.FillCount() == 1);
}

void TestDelayAck() {
  FaultConfig cfg;
  cfg.seed = 3;
  cfg.ack_delay_ms = 300;
  Recorder r;
  SimOrderBackend b(FillMode::kConservative, {"2330"}, cfg);
  r.Wire(b);
  OpenContinuous(b);
  auto t0 = std::chrono::steady_clock::now();
  b.Submit(Order("k-1", Side::kBuy, 10000, 1000));
  CHECK(r.AckCount() == 0);  // not delivered synchronously
  // Wait for the delayed ack.
  while (r.AckCount() == 0 &&
         std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(3000))
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
          .count();
  CHECK(r.AckCount() == 1);
  CHECK(elapsed >= 300);
}

void TestPartialFill() {
  FaultConfig cfg;
  cfg.seed = 5;
  cfg.partial_fill = 4;
  Recorder r;
  SimOrderBackend b(FillMode::kConservative, {"2330"}, cfg);
  r.Wire(b);
  OpenContinuous(b);
  b.Submit(Order("k-1", Side::kBuy, 10000, 1000));
  Trade(b);
  std::lock_guard<std::mutex> l(r.mu);
  CHECK(r.fills.size() == 4);
  long sum = 0;
  for (const auto& f : r.fills) {
    sum += f.shares;
    CHECK(f.price == 10000);
  }
  CHECK(sum == 1000);
}

void TestSeedDeterminism() {
  // Two injectors, same seed => identical reject/drop streams. Draw order per
  // submit is fixed (reject, then drop), both observed synchronously (no delay).
  FaultConfig cfg;
  cfg.seed = 42;
  cfg.reject_rate = 0.5;
  cfg.ack_drop_rate = 0.5;
  auto run = [](FaultConfig c) {
    std::vector<char> rejects, delivered;
    FaultInjector inj(c);
    for (int i = 0; i < 64; ++i) {
      inj.NoteSubmit();
      rejects.push_back(inj.DrawReject() ? 1 : 0);
      bool got = false;
      inj.OnAck("id", true, "",
                [&got](const std::string&, bool, const std::string&) { got = true; });
      delivered.push_back(got ? 1 : 0);
    }
    return std::make_pair(rejects, delivered);
  };
  auto a = run(cfg);
  auto b = run(cfg);
  CHECK(a.first == b.first);
  CHECK(a.second == b.second);
  // Both streams must be non-trivial (a 0.5 rate over 64 draws mixes outcomes).
  CHECK(a.first != std::vector<char>(64, 0) && a.first != std::vector<char>(64, 1));
  // A different seed must diverge somewhere.
  FaultConfig cfg2 = cfg;
  cfg2.seed = 43;
  auto c = run(cfg2);
  CHECK(a.first != c.first || a.second != c.second);
}

void TestStopDropsPendingAck() {
  // A delayed ack still queued when Stop() runs must never be delivered (the
  // worker's wait_until can wake by timeout on a past-due deadline right as Stop
  // sets stop_). Honors the header contract "Stop drops any still-pending acks".
  FaultConfig cfg;
  cfg.seed = 11;
  cfg.ack_delay_ms = 100000;  // far in the future so it stays queued
  std::atomic<int> acks{0};
  {
    FaultInjector inj(cfg);
    inj.OnAck("k-1", true, "", [&acks](const std::string&, bool, const std::string&) { ++acks; });
    inj.Stop();  // drops the queued ack
  }
  CHECK(acks.load() == 0);
}

}  // namespace

int main() {
  TestAllOffIdentical();
  TestReject();
  TestDropAck();
  TestDelayAck();
  TestPartialFill();
  TestSeedDeterminism();
  TestStopDropsPendingAck();

  if (g_failures == 0) {
    std::printf("test_sim_faults: OK\n");
    return 0;
  }
  std::printf("test_sim_faults: FAILED %d check(s)\n", g_failures);
  return 1;
}
