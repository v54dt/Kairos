// ProbQueueModel: deterministic queue advancement on trades-at-price and
// proportional book-decrease, with pending_trade_vol reconciliation to avoid
// double-counting a just-printed trade. Arithmetic asserted exactly.

#include <cstdio>
#include <string>
#include <vector>

#include "fill_model.h"
#include "quote_book.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

namespace {

struct FillRec {
  std::string id;
  long shares;
  Cents price;
};

struct Capture {
  std::vector<FillRec> fills;
};

SymbolFillModel Make(Capture* c) {
  return SymbolFillModel(
      "2330", FillMode::kProbQueue, [](const std::string&, bool, const std::string&) {},
      [c](const std::string& id, const Fill& f) { c->fills.push_back({id, f.shares, f.price}); },
      [](const std::string&, bool) {});
}

TopOfBook Book(std::vector<Level> bids, std::vector<Level> asks) {
  TopOfBook t;
  for (auto& b : bids) {
    if (t.n_bids >= TopOfBook::kMaxLevels) break;
    t.bids[t.n_bids++] = b;
  }
  for (auto& a : asks) {
    if (t.n_asks >= TopOfBook::kMaxLevels) break;
    t.asks[t.n_asks++] = a;
  }
  t.valid = true;
  return t;
}

SimOrder Buy(const std::string& id, Cents price, long shares) {
  SimOrder o;
  o.id = id;
  o.symbol = "2330";
  o.side = Side::kBuy;
  o.board = Board::kRoundLot;
  o.price = price;
  o.shares = shares;
  return o;
}

}  // namespace

int main() {
  // Trades at the order's price: no fill until queue_ahead is exhausted.
  {
    Capture c;
    auto m = Make(&c);
    m.OnBook(Book({{10000, 100}}, {{10100, 100}}), 1);
    m.Submit(Buy("b1", 10000, 50));  // queue_ahead = 100
    m.OnTrade(10000, 30, 2, false);  // 30 < 100 ahead -> no fill, ahead 70
    CHECK(c.fills.empty());
    m.OnTrade(10000, 80, 3, false);  // 80 > 70 ahead -> fill 10, ahead 0
    CHECK(c.fills.size() == 1 && c.fills[0].shares == 10 && c.fills[0].price == 10000);
    m.OnTrade(10000, 100, 4, false);  // ahead 0 -> fill remaining 40
    CHECK(c.fills.size() == 2 && c.fills[1].shares == 40);
    CHECK(!m.HasResting());
  }

  // Proportional advancement on a bare book decrease (no trade): floor(D_eff*q/Dprev).
  {
    Capture c;
    auto m = Make(&c);
    m.OnBook(Book({{10000, 100}}, {{10100, 100}}), 1);
    m.Submit(Buy("b1", 10000, 50));  // ahead 100
    m.OnBook(Book({{10000, 60}}, {{10100, 100}}),
             2);  // -40 -> ahead floor(40*100/100)=40 off => 60
    m.OnBook(Book({{10000, 30}}, {{10100, 100}}), 3);  // -30 -> floor(30*60/60)=30 off => ahead 30
    CHECK(c.fills.empty());                            // book decrease never fills on its own
    m.OnTrade(10000, 50, 4, false);                    // 50 > 30 ahead -> fill 20
    CHECK(c.fills.size() == 1 && c.fills[0].shares == 20);
  }

  // Mixed trade + shrink: the book decrease reflecting the just-printed trade must
  // NOT double-advance the queue (pending_trade_vol absorbs it).
  {
    Capture c;
    auto m = Make(&c);
    m.OnBook(Book({{10000, 100}}, {{10100, 100}}), 1);
    m.Submit(Buy("b1", 10000, 50));                    // ahead 100
    m.OnTrade(10000, 40, 2, false);                    // ahead 60, pending 40, no fill
    m.OnBook(Book({{10000, 60}}, {{10100, 100}}), 3);  // -40 == pending -> D_eff 0, ahead stays 60
    m.OnBook(Book({{10000, 40}}, {{10100, 100}}),
             4);  // -20 genuine -> floor(20*60/60)=20 => ahead 40
    CHECK(c.fills.empty());
    m.OnTrade(10000, 50, 5, false);  // 50 > 40 ahead -> fill 10
    CHECK(c.fills.size() == 1 && c.fills[0].shares == 10);
  }

  // Marketable-on-arrival portion fills immediately, remainder rests with queue
  // initialized from the displayed volume at the resting level (0 when not shown).
  {
    Capture c;
    auto m = Make(&c);
    m.OnBook(Book({{9990, 100}}, {{10000, 30}}), 1);
    m.Submit(Buy("b1", 10000, 50));  // walk 30 @100.00, rest 20 with queue_ahead 0
    CHECK(c.fills.size() == 1 && c.fills[0].shares == 30 && c.fills[0].price == 10000);
    m.OnTrade(10000, 5, 2, false);  // ahead 0 -> fill 5
    CHECK(c.fills.size() == 2 && c.fills[1].shares == 5);
  }

  // Two orders stacked at the same price share one trade's post-queue volume in
  // time priority; the trade volume is never double-counted across them.
  {
    Capture c;
    auto m = Make(&c);
    m.OnBook(Book({{10000, 100}}, {{10100, 100}}), 1);
    m.Submit(Buy("b1", 10000, 50));   // queue_ahead 100
    m.Submit(Buy("b2", 10000, 50));   // queue_ahead 100
    m.OnTrade(10000, 150, 2, false);  // 150-100 = 50 past queue: b1 fills 50, b2 0
    CHECK(c.fills.size() == 1 && c.fills[0].id == "b1" && c.fills[0].shares == 50);
    m.OnTrade(10000, 10, 3, false);  // queues now 0: b2 fills 10 (front of queue)
    CHECK(c.fills.size() == 2 && c.fills[1].id == "b2" && c.fills[1].shares == 10);
  }

  if (g_failures == 0) {
    std::printf("test_prob_queue: OK\n");
    return 0;
  }
  std::printf("test_prob_queue: FAILED %d check(s)\n", g_failures);
  return 1;
}
