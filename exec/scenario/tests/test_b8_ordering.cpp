// The production concords feed delivers, for a combined quotation, the (post-trade)
// depth Quote FIRST then the Trade (sidecar feed.cpp:249 then :254). The A4
// kProbQueue model's pending_trade_vol reconciliation assumes the OPPOSITE order
// ("Trade ... reconciles the NEXT book decrease", fill_model.h). QueueSimBackend
// buffers each post-trade Quote until after its paired Trade, so both feed orders
// now reconcile to the SAME (correct) queue-accounted fill instead of the real-feed
// order double-advancing the queue into an over-optimistic paper fill.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "order_backend.h"
#include "queue_sim_backend.h"
#include "quote_book.h"

using namespace kairos::exec;
namespace {
constexpr std::int64_t kT = 7200LL * 1000000;  // 10:00 Taipei, continuous
TopOfBook Bid(long vol, std::int64_t ts) {
  TopOfBook t;
  t.bids[0] = {10000, vol};
  t.n_bids = 1;
  t.asks[0] = {10010, 1000};
  t.n_asks = 1;
  t.quote_ts_us = ts;
  t.valid = true;
  return t;
}
long Total(const std::vector<long>& v) {
  long n = 0;
  for (long x : v) n += x;
  return n;
}
void Wire(OrderBackend* b, std::vector<long>* out) {
  b->SetCallbacks([](const std::string&, bool, const std::string&) {},
                  [out](const std::string&, const Fill& f) { out->push_back(f.shares); },
                  [](const std::string&, bool) {});
}
OrderSubmitMsg Buy() {
  return {"o1", "2330", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash", "ROD", 10000, 3000};
}
}  // namespace

int main() {
  // Market history (identical facts): I rest BUY 3000 @ 10000 behind 2000 displayed.
  // Then 1500 trades at 10000 (book 2000 -> 500). Then 600 trades at 10000.
  // Correct queue accounting fills me only for volume beyond my 2000 queue.

  // (A) Model's ASSUMED order: Trade THEN the post-trade book decrease.
  std::vector<long> a;
  {
    QueueSimBackend sim(FillMode::kProbQueue, {"2330"});
    Wire(&sim, &a);
    sim.OnMarketBook("2330", Bid(2000, kT), kT);
    sim.Submit(Buy());
    sim.OnMarketTrade("2330", Trade{10000, 1500, kT + 1, false}, kT + 1);
    sim.OnMarketBook("2330", Bid(500, kT + 2), kT + 2);  // decrease reflects the 1500 print
    sim.OnMarketTrade("2330", Trade{10000, 600, kT + 3, false}, kT + 3);
  }

  // (B) REAL concords feed order: post-trade book Quote FIRST, then the Trade.
  std::vector<long> b;
  {
    QueueSimBackend sim(FillMode::kProbQueue, {"2330"});
    Wire(&sim, &b);
    sim.OnMarketBook("2330", Bid(2000, kT), kT);
    sim.Submit(Buy());
    sim.OnMarketBook("2330", Bid(500, kT + 1), kT + 1);  // depth already reduced by the 1500 print
    sim.OnMarketTrade("2330", Trade{10000, 1500, kT + 1, false}, kT + 1);
    sim.OnMarketBook("2330", Bid(0, kT + 2), kT + 2);  // depth reduced by the 600 print
    sim.OnMarketTrade("2330", Trade{10000, 600, kT + 3, false}, kT + 3);
  }

  std::printf("trade-first (model's assumption): filled %ld / 3000\n", Total(a));
  std::printf("book-first  (real concords feed): filled %ld / 3000\n", Total(b));
  // Correct queue accounting: 2000 queue ahead, then 1500+600=2100 traded at price
  // fills only the 100 beyond the queue. Buffering makes both orders agree on it.
  int failures = 0;
  if (Total(a) != 100) {
    std::printf("FAIL trade-first filled %ld, want 100\n", Total(a));
    ++failures;
  }
  if (Total(b) != Total(a)) {
    std::printf("FAIL book-first filled %ld, want %ld (feed order must not change fills)\n",
                Total(b), Total(a));
    ++failures;
  }
  if (failures == 0) {
    std::printf("test_b8_ordering: OK\n");
    return 0;
  }
  std::printf("test_b8_ordering: FAILED %d check(s)\n", failures);
  return 1;
}
