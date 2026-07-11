// Contrasts the in-process queue-sim paper backend against the instant-fill
// PaperOrderBackend on the same synthetic book+trade tape: a passive BUY queued
// behind displayed size fills only partially (queue model engaged), while the
// instant backend fills it in full immediately. Also proves a trade-through fills
// the resting order in full, and that the queue-sim fill sequence is deterministic
// across two independent instances driven with the identical tape.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "order_backend.h"
#include "queue_sim_backend.h"
#include "quote_book.h"
#include "test_check.h"

using namespace kairos::exec;

namespace {

// 10:00 Taipei (02:00 UTC) — continuous session, so submits route to the queue
// model rather than a call auction.
constexpr std::int64_t kCont = 7200LL * 1000000;

struct FillRec {
  std::string id;
  long shares;
  Cents price;
  bool operator==(const FillRec& o) const {
    return id == o.id && shares == o.shares && price == o.price;
  }
};

// Two-sided book: bid at `bid_px` with `bid_vol` displayed, ask one tick above so
// a BUY at `bid_px` is passive (not marketable on arrival).
TopOfBook Book(Cents bid_px, long bid_vol, Cents ask_px, std::int64_t ts_us) {
  TopOfBook t;
  t.bids[0] = {bid_px, bid_vol};
  t.n_bids = 1;
  t.asks[0] = {ask_px, 1000};
  t.n_asks = 1;
  t.quote_ts_us = ts_us;
  t.valid = true;
  return t;
}

Trade Print(Cents price, long vol, std::int64_t ts_us) { return Trade{price, vol, ts_us, false}; }

OrderSubmitMsg BuyOrder(const std::string& id, Cents price, long shares) {
  return {id, "2330", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash", "ROD", price, shares};
}

OrderSubmitMsg OddLotBuy(const std::string& id, Cents price, long shares) {
  return {id, "2330", Market::kTse, Board::kOddLot, Side::kBuy, "Cash", "ROD", price, shares};
}

long TotalShares(const std::vector<FillRec>& fills) {
  long n = 0;
  for (const auto& f : fills) n += f.shares;
  return n;
}

void Record(OrderBackend* b, std::vector<FillRec>* out) {
  b->SetCallbacks(
      [](const std::string&, bool, const std::string&) {},
      [out](const std::string& id, const Fill& f) { out->push_back({id, f.shares, f.price}); },
      [](const std::string&, bool) {});
}

// Passive BUY behind 1000 displayed; a 1500-share print at the limit clears the
// 1000 ahead and fills only the 500 beyond it — a partial, queued fill.
void TestQueueVsInstantPartial() {
  const Cents kP = 10000;    // 100.00
  const Cents kAsk = 10010;  // 100.10
  const long kOrder = 2000;

  std::vector<FillRec> sim_fills;
  QueueSimBackend sim(FillMode::kProbQueue, {"2330"});
  Record(&sim, &sim_fills);
  CHECK(sim.WantsMarketTrades());
  CHECK(sim.Connect());
  sim.OnMarketBook("2330", Book(kP, 1000, kAsk, kCont), kCont);
  sim.Submit(BuyOrder("q1", kP, kOrder));
  sim.OnMarketTrade("2330", Print(kP, 1500, kCont + 1000), kCont + 1000);

  std::vector<FillRec> paper_fills;
  PaperOrderBackend paper;
  Record(&paper, &paper_fills);
  CHECK(!paper.WantsMarketTrades());
  paper.OnMarketBook("2330", Book(kP, 1000, kAsk, kCont), kCont);
  paper.Submit(BuyOrder("q1", kP, kOrder));
  paper.OnMarketTrade("2330", Print(kP, 1500, kCont + 1000), kCont + 1000);

  long sim_total = TotalShares(sim_fills);
  long paper_total = TotalShares(paper_fills);
  std::printf("queue-sim: filled %ld / %ld sh (queued behind 1000)\n", sim_total, kOrder);
  std::printf("instant-paper: filled %ld / %ld sh\n", paper_total, kOrder);
  CHECK(sim_total == 500);       // 1500 print - 1000 queue_ahead
  CHECK(sim_total < kOrder);     // NOT an instant full fill
  CHECK(paper_total == kOrder);  // instant paper fills the whole order at the limit
}

// A trade strictly through the limit sweeps the level: the resting order fills in
// full at its limit regardless of queue position.
void TestTradeThroughFillsFull() {
  const Cents kP = 10000;
  const Cents kAsk = 10010;
  const long kOrder = 2000;

  std::vector<FillRec> fills;
  QueueSimBackend sim(FillMode::kProbQueue, {"2330"});
  Record(&sim, &fills);
  sim.OnMarketBook("2330", Book(kP, 5000, kAsk, kCont), kCont);
  sim.Submit(BuyOrder("t1", kP, kOrder));
  sim.OnMarketTrade("2330", Print(kP - 10, 100, kCont + 1000), kCont + 1000);  // through P

  std::printf("trade-through: filled %ld / %ld sh\n", TotalShares(fills), kOrder);
  CHECK(TotalShares(fills) == kOrder);
}

// Same tape into two independent instances -> element-wise identical fills.
void TestDeterminism() {
  const Cents kP = 10000;
  const Cents kAsk = 10010;

  auto run = [&](std::vector<FillRec>* out) {
    QueueSimBackend sim(FillMode::kProbQueue, {"2330"});
    Record(&sim, out);
    sim.OnMarketBook("2330", Book(kP, 800, kAsk, kCont), kCont);
    sim.Submit(BuyOrder("d1", kP, 3000));
    sim.OnMarketTrade("2330", Print(kP, 1200, kCont + 1000), kCont + 1000);
    sim.OnMarketTrade("2330", Print(kP, 1500, kCont + 2000), kCont + 2000);
  };

  std::vector<FillRec> a, b;
  run(&a);
  run(&b);
  CHECK(a.size() == b.size());
  CHECK(!a.empty());
  CHECK(a == b);
}

// Odd-lot is the scenario default board and the round-lot fill engine rejects it,
// so the default paper backend must still ack and fill an odd-lot order (instantly)
// rather than reject-storm it.
void TestOddLotFillsInsteadOfReject() {
  const Cents kP = 10000;

  std::vector<FillRec> fills;
  std::vector<std::string> rejects;
  QueueSimBackend sim(FillMode::kProbQueue, {"2330"});
  sim.SetCallbacks(
      [&rejects](const std::string& id, bool ok, const std::string&) {
        if (!ok) rejects.push_back(id);
      },
      [&fills](const std::string& id, const Fill& f) { fills.push_back({id, f.shares, f.price}); },
      [](const std::string&, bool) {});
  sim.OnMarketBook("2330", Book(kP, 1000, kP + 10, kCont), kCont);
  sim.Submit(OddLotBuy("odd1", kP, 500));

  std::printf("odd-lot paper: filled %ld sh, %zu reject(s)\n", TotalShares(fills), rejects.size());
  CHECK(rejects.empty());            // NOT rejected as "odd-lot board not supported"
  CHECK(TotalShares(fills) == 500);  // instant full fill at the limit
  CHECK(fills.size() == 1 && fills[0].price == kP);
}

}  // namespace

int main() {
  TestQueueVsInstantPartial();
  TestTradeThroughFillsFull();
  TestDeterminism();
  TestOddLotFillsInsteadOfReject();
  if (g_failures == 0) {
    std::printf("test_queue_sim_paper: OK\n");
    return 0;
  }
  std::printf("test_queue_sim_paper: FAILED %d check(s)\n", g_failures);
  return 1;
}
