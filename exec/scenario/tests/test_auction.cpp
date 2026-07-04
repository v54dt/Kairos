// Call-auction module: session-phase boundaries, single crossing-price
// computation with tie-breaks, price-time-priority allocation, and the FillEngine
// integration (orders held in-window, opening/closing match at the boundary ts,
// 3.5% 延緩收盤 delay on/off, unmatched remainder -> continuous). Event-time only.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "auction.h"
#include "fill_engine.h"
#include "quote_book.h"
#include "session_schedule.h"

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

// CLOCK_REALTIME microseconds for a Taipei local wall time (UTC+8).
std::int64_t Us(int hh, int mm, int ss = 0) {
  long local = hh * 3600 + mm * 60 + ss;
  return static_cast<std::int64_t>(local - 8 * 3600) * 1000000;
}

SimOrder Ord(const std::string& id, Side side, Cents price, long shares, std::int64_t ts) {
  SimOrder o;
  o.id = id;
  o.symbol = "2330";
  o.side = side;
  o.board = Board::kRoundLot;
  o.price = price;
  o.shares = shares;
  o.place_ts_us = ts;
  return o;
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

struct FillRec {
  std::string id;
  long shares;
  Cents price;
};
struct Capture {
  std::vector<FillRec> fills;
  long shares_for(const std::string& id) const {
    long s = 0;
    for (auto& f : fills)
      if (f.id == id) s += f.shares;
    return s;
  }
};

FillEngine MakeEngine(Capture* c) {
  return FillEngine(
      FillMode::kConservative, [](const std::string&, bool, const std::string&) {},
      [c](const std::string& id, const Fill& f) { c->fills.push_back({id, f.shares, f.price}); },
      [](const std::string&, bool) {});
}

void TestPhases() {
  CHECK(PhaseForHhmm(859) == SessionPhase::kPreOpen);
  CHECK(PhaseForHhmm(kOpenHhmm) == SessionPhase::kContinuous);  // exactly 09:00
  CHECK(PhaseForHhmm(1000) == SessionPhase::kContinuous);
  CHECK(PhaseForHhmm(kCloseWindowStartHhmm) == SessionPhase::kClosingAuction);  // exactly 13:25
  CHECK(PhaseForHhmm(1329) == SessionPhase::kClosingAuction);
  CHECK(PhaseForHhmm(kCloseHhmm) == SessionPhase::kClosed);  // exactly 13:30
  CHECK(HhmmFromUs(Us(13, 25, 0)) == 1325);
  CHECK(HhmmFromUs(Us(13, 30, 0)) == 1330);
  CHECK(HhmmFromUs(Us(9, 0, 0)) == 900);
  CHECK(AddMinutesHhmm(1330, 3) == 1333);
}

void TestCrossing() {
  // Max-executable-volume price.
  {
    AuctionEngine a;
    a.Add(Ord("b1", Side::kBuy, 10100, 100, 1));
    a.Add(Ord("b2", Side::kBuy, 10000, 100, 2));
    a.Add(Ord("s1", Side::kSell, 9900, 100, 3));
    a.Add(Ord("s2", Side::kSell, 10000, 100, 4));
    auto x = a.ComputeCross();
    CHECK(x.crossed && x.price == 10000 && x.volume == 200);
  }
  // Tie on exec -> minimize imbalance.
  {
    AuctionEngine a;
    a.Add(Ord("b1", Side::kBuy, 10100, 100, 1));
    a.Add(Ord("b2", Side::kBuy, 10000, 100, 2));
    a.Add(Ord("s1", Side::kSell, 10000, 100, 3));
    auto x = a.ComputeCross();  // exec 100 at both 10000 (imb 100) and 10100 (imb 0)
    CHECK(x.crossed && x.price == 10100);
  }
  // Tie on exec + imbalance -> reference proximity, else higher price.
  {
    AuctionEngine a;
    a.Add(Ord("b1", Side::kBuy, 10200, 100, 1));
    a.Add(Ord("s1", Side::kSell, 10000, 100, 2));
    CHECK(a.ComputeCross().price == 10200);  // no ref -> higher price
    a.SetReference(10000);
    CHECK(a.ComputeCross().price == 10000);  // closest to ref
    a.SetReference(10250);
    CHECK(a.ComputeCross().price == 10200);
  }
  // Price-time-priority allocation of marginal shares at the cross.
  {
    AuctionEngine a;
    a.Add(Ord("A", Side::kBuy, 10000, 100, 1));
    a.Add(Ord("B", Side::kBuy, 10000, 100, 2));
    a.Add(Ord("S", Side::kSell, 10000, 150, 3));
    auto fills = a.Match();  // exec 150: A(100) before B(50) by time; S 150
    long fa = 0, fb = 0, fs = 0;
    for (auto& f : fills) {
      CHECK(f.price == 10000);
      if (f.id == "A") fa += f.shares;
      if (f.id == "B") fb += f.shares;
      if (f.id == "S") fs += f.shares;
    }
    CHECK(fa == 100 && fb == 50 && fs == 150);
  }
  // 3.5% band.
  {
    AuctionEngine a;
    a.SetReference(10000);
    a.Add(Ord("b", Side::kBuy, 10400, 100, 1));
    a.Add(Ord("s", Side::kSell, 10400, 100, 2));
    CHECK(a.DeviatesBeyondBand());  // 4% > 3.5%
  }
  {
    AuctionEngine a;
    a.SetReference(10000);
    a.Add(Ord("b", Side::kBuy, 10300, 100, 1));
    a.Add(Ord("s", Side::kSell, 10300, 100, 2));
    CHECK(!a.DeviatesBeyondBand());  // 3% < 3.5%
  }
}

void TestOpeningAuction() {
  Capture c;
  FillEngine e = MakeEngine(&c);
  e.AddSymbol("2330");
  // Pre-open accumulation: no fills while the window is open.
  e.Submit(Ord("b", Side::kBuy, 10000, 200, Us(8, 59, 0)));  // over-supplied buy
  e.Submit(Ord("s", Side::kSell, 10000, 100, Us(8, 59, 30)));
  e.OnTrade("2330", 9900, 100, Us(8, 59, 45), false);  // pre-open: frozen, no fill
  CHECK(c.fills.empty());
  // Match at exactly 09:00; remainder of the buy carries to continuous.
  e.OnBook("2330", Book({{9990, 100}}, {{10100, 100}}), Us(9, 0, 0));
  CHECK(c.shares_for("b") == 100 && c.shares_for("s") == 100);
  // Continuous now live: a trade strictly through 100.00 fills the resting 100.
  e.OnTrade("2330", 9900, 100, Us(9, 0, 5), false);
  CHECK(c.shares_for("b") == 200);
}

void TestClosingAuctionNoDelay() {
  Capture c;
  FillEngine e = MakeEngine(&c);
  e.AddSymbol("2330");
  e.OnBook("2330", Book({{9990, 100}}, {{10100, 100}}), Us(10, 0, 0));
  e.OnTrade("2330", 10000, 10, Us(10, 0, 1), false);  // reference := 100.00
  // Closing window opens exactly 13:25; orders accumulate, no fill.
  e.Submit(Ord("b", Side::kBuy, 10000, 100, Us(13, 25, 0)));
  e.Submit(Ord("s", Side::kSell, 10000, 100, Us(13, 25, 30)));
  CHECK(c.fills.empty());
  // Cross 100.00 == reference -> no delay; match at exactly 13:30.
  e.OnBook("2330", Book({{10000, 100}}, {{10100, 100}}), Us(13, 30, 0));
  CHECK(c.shares_for("b") == 100 && c.shares_for("s") == 100);
}

void TestClosingAuctionDelay() {
  Capture c;
  FillEngine e = MakeEngine(&c);
  e.AddSymbol("2330");
  e.OnTrade("2330", 10000, 10, Us(10, 0, 1), false);          // reference := 100.00
  e.Submit(Ord("b", Side::kBuy, 10500, 100, Us(13, 25, 0)));  // cross 105.00 == +5%
  e.Submit(Ord("s", Side::kSell, 10500, 100, Us(13, 25, 30)));
  // 13:30 would deviate > 3.5% -> delayed once, NO match yet.
  e.OnBook("2330", Book({{10500, 100}}, {{10600, 100}}), Us(13, 30, 0));
  CHECK(c.fills.empty());
  // Extended close at 13:33 matches unconditionally.
  e.OnBook("2330", Book({{10500, 100}}, {{10600, 100}}), Us(13, 33, 0));
  CHECK(c.shares_for("b") == 100 && c.shares_for("s") == 100);
}

// A delayed close whose tape ends before the extended-close boundary: the acked
// closing orders must be resolved by Finalize (end of tape), not silently lost.
void TestClosingAuctionFinalizeFlush() {
  Capture c;
  FillEngine e = MakeEngine(&c);
  e.AddSymbol("2330");
  e.OnTrade("2330", 10000, 10, Us(10, 0, 1), false);          // reference := 100.00
  e.Submit(Ord("b", Side::kBuy, 10500, 100, Us(13, 25, 0)));  // cross 105.00 == +5%
  e.Submit(Ord("s", Side::kSell, 10500, 100, Us(13, 25, 30)));
  // First post-boundary event arrives late (13:35): deviates > 3.5% -> delayed to
  // 13:33, match deferred; the tape then ends with no further event.
  e.OnBook("2330", Book({{10500, 100}}, {{10600, 100}}), Us(13, 35, 0));
  CHECK(c.fills.empty());
  e.Finalize();  // end of tape -> forced closing match
  CHECK(c.shares_for("b") == 100 && c.shares_for("s") == 100);
  e.Finalize();  // idempotent: no double fill
  CHECK(c.shares_for("b") == 100 && c.shares_for("s") == 100);
}

}  // namespace

int main() {
  TestPhases();
  TestCrossing();
  TestOpeningAuction();
  TestClosingAuctionNoDelay();
  TestClosingAuctionDelay();
  TestClosingAuctionFinalizeFlush();

  if (g_failures == 0) {
    std::printf("test_auction: OK\n");
    return 0;
  }
  std::printf("test_auction: FAILED %d check(s)\n", g_failures);
  return 1;
}
