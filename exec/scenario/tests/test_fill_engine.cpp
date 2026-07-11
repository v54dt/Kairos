// FillEngine multiplexer: odd-lot/unknown-symbol rejection, per-symbol
// isolation, partial-fill accounting, cancel semantics, and determinism (same
// event+order tape twice -> byte-identical fill sequence). All timestamps are in
// the continuous session (10:00 Taipei) so no auction phase is triggered here.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "fill_engine.h"
#include "quote_book.h"
#include "test_check.h"

using namespace kairos::exec;

namespace {

// 10:00:00 Taipei == 02:00:00 UTC == 7200s past UTC midnight; add a sub-second
// offset so events stay within the continuous session, minute-granular.
constexpr std::int64_t kCont = 7200LL * 1000000;
std::int64_t At(long micros) { return kCont + micros; }

struct AckRec {
  std::string id;
  bool ok;
  std::string err;
};
struct FillRec {
  std::string id;
  long shares;
  Cents price;
};

struct Capture {
  std::vector<AckRec> acks;
  std::vector<FillRec> fills;
  std::vector<std::pair<std::string, bool>> cancels;

  bool operator==(const Capture& o) const {
    if (fills.size() != o.fills.size()) return false;
    for (size_t i = 0; i < fills.size(); ++i) {
      if (fills[i].id != o.fills[i].id || fills[i].shares != o.fills[i].shares ||
          fills[i].price != o.fills[i].price)
        return false;
    }
    return true;
  }
};

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

SimOrder Buy(const std::string& id, const std::string& sym, Cents price, long shares,
             Board board = Board::kRoundLot) {
  SimOrder o;
  o.id = id;
  o.symbol = sym;
  o.side = Side::kBuy;
  o.board = board;
  o.price = price;
  o.shares = shares;
  o.place_ts_us = kCont;
  return o;
}

// CLOCK_REALTIME microseconds for a Taipei local wall time (UTC+8).
std::int64_t Us(int hh, int mm, int ss = 0) {
  long local = hh * 3600 + mm * 60 + ss;
  return static_cast<std::int64_t>(local - 8 * 3600) * 1000000;
}

SimOrder Order(const std::string& id, const std::string& sym, Side side, Cents price, long shares,
               std::int64_t ts) {
  SimOrder o;
  o.id = id;
  o.symbol = sym;
  o.side = side;
  o.price = price;
  o.shares = shares;
  o.place_ts_us = ts;
  return o;
}

FillEngine Make(Capture* c, FillMode mode = FillMode::kConservative) {
  return FillEngine(
      mode,
      [c](const std::string& id, bool ok, const std::string& e) { c->acks.push_back({id, ok, e}); },
      [c](const std::string& id, const Fill& f) { c->fills.push_back({id, f.shares, f.price}); },
      [c](const std::string& id, bool ok) { c->cancels.push_back({id, ok}); });
}

// Deterministic scripted tape: books, trades, submits replayed in order.
Capture RunTape() {
  Capture c;
  FillEngine e = Make(&c, FillMode::kProbQueue);
  e.AddSymbol("2330");
  e.AddSymbol("2317");
  e.OnBook("2330", Book({{58000, 100}}, {{58100, 100}}), At(1));
  e.OnBook("2317", Book({{12000, 200}}, {{12050, 200}}), At(2));
  e.Submit(Buy("a", "2330", 58000, 50));        // queue_ahead 100
  e.Submit(Buy("b", "2317", 12000, 300));       // queue_ahead 200
  e.OnTrade("2330", 58000, 120, At(3), false);  // fill a: 120-100=20
  e.OnTrade("2317", 12000, 260, At(4), false);  // fill b: 260-200=60
  e.OnBook("2330", Book({{58000, 0}}, {{58100, 100}}), At(5));
  e.OnTrade("2330", 58000, 100, At(6), false);  // fill a: remaining 30
  e.OnTrade("2317", 12000, 500, At(7), false);  // fill b: remaining
  return c;
}

}  // namespace

int main() {
  // Odd-lot board is rejected with no fill.
  {
    Capture c;
    FillEngine e = Make(&c);
    e.AddSymbol("2330");
    e.OnBook("2330", Book({{10000, 100}}, {{10100, 100}}), At(1));
    e.Submit(Buy("odd", "2330", 10000, 500, Board::kOddLot));
    CHECK(c.acks.size() == 1 && !c.acks[0].ok);
    CHECK(c.fills.empty());
  }

  // Unknown symbol rejected.
  {
    Capture c;
    FillEngine e = Make(&c);
    e.AddSymbol("2330");
    e.Submit(Buy("x", "9999", 10000, 1000));
    CHECK(c.acks.size() == 1 && !c.acks[0].ok && c.acks[0].err == "unknown symbol");
  }

  // Per-symbol isolation: a trade on 2330 must not fill an order on 2317.
  {
    Capture c;
    FillEngine e = Make(&c);
    e.AddSymbol("2330");
    e.AddSymbol("2317");
    e.OnBook("2330", Book({{10000, 100}}, {{10100, 100}}), At(1));
    e.OnBook("2317", Book({{20000, 100}}, {{20100, 100}}), At(2));
    e.Submit(Buy("a", "2330", 10000, 50));
    e.OnTrade("2317", 9900, 100, At(3), false);  // different symbol
    CHECK(c.fills.empty());
    e.OnTrade("2330", 9900, 100, At(4), false);
    CHECK(c.fills.size() == 1 && c.fills[0].id == "a");
  }

  // Partial-fill accounting + completion: order removed when fully filled.
  {
    Capture c;
    FillEngine e = Make(&c);
    e.AddSymbol("2330");
    e.OnBook("2330", Book({{10000, 100}}, {{10100, 100}}), At(1));
    e.Submit(Buy("a", "2330", 10000, 100));
    e.OnTrade("2330", 9900, 30, At(2), false);  // partial 30
    e.OnTrade("2330", 9900, 40, At(3), false);  // partial 40
    e.OnTrade("2330", 9900, 90, At(4), false);  // remaining 30 (capped)
    CHECK(c.fills.size() == 3);
    if (c.fills.size() == 3) {
      CHECK(c.fills[0].shares == 30 && c.fills[1].shares == 40 && c.fills[2].shares == 30);
    }
    e.OnTrade("2330", 9900, 100, At(5), false);  // completed order must not fill again
    CHECK(c.fills.size() == 3);
  }

  // Cancel before any fill -> ok=true.
  {
    Capture c;
    FillEngine e = Make(&c);
    e.AddSymbol("2330");
    e.OnBook("2330", Book({{10000, 100}}, {{10100, 100}}), At(1));
    e.Submit(Buy("a", "2330", 10000, 100));
    e.Cancel("a", At(2));
    CHECK(c.cancels.size() == 1 && c.cancels[0].first == "a" && c.cancels[0].second);
    e.OnTrade("2330", 9900, 100, At(3), false);  // no fill after cancel
    CHECK(c.fills.empty());
  }

  // Cancel after partial fill cancels the remainder; no post-cancel fill.
  {
    Capture c;
    FillEngine e = Make(&c);
    e.AddSymbol("2330");
    e.OnBook("2330", Book({{10000, 100}}, {{10100, 100}}), At(1));
    e.Submit(Buy("a", "2330", 10000, 100));
    e.OnTrade("2330", 9900, 30, At(2), false);  // partial 30
    e.Cancel("a", At(3));
    CHECK(c.cancels.size() == 1 && c.cancels[0].second);
    e.OnTrade("2330", 9900, 100, At(4), false);
    CHECK(c.fills.size() == 1);  // only the pre-cancel partial
  }

  // Cancel after full fill -> ok=false (nothing to cancel).
  {
    Capture c;
    FillEngine e = Make(&c);
    e.AddSymbol("2330");
    e.OnBook("2330", Book({{10000, 100}}, {{10100, 100}}), At(1));
    e.Submit(Buy("a", "2330", 10000, 50));
    e.OnTrade("2330", 9900, 50, At(2), false);
    CHECK(c.fills.size() == 1);
    e.Cancel("a", At(3));
    CHECK(c.cancels.size() == 1 && !c.cancels[0].second);
  }

  // A4 fidelity: a resting continuous order still alive when the closing window
  // opens is frozen (not migrated into the closing auction, A8) and MUST receive a
  // terminal expiry cancel at the close -- never left live forever.
  {
    Capture c;
    FillEngine e = Make(&c);
    e.AddSymbol("2330");
    e.OnTrade("2330", 10000, 10, Us(10, 0, 0), false);  // reference := 100.00
    e.Submit(Order("rest", "2330", Side::kBuy, 10000, 1000, Us(10, 0, 1)));
    CHECK(c.acks.size() == 1 && c.acks[0].ok);
    // 5000-lot print at 100.00 in the closing window: continuous matching is frozen.
    e.OnTrade("2330", 10000, 5000, Us(13, 26, 0), false);
    CHECK(c.fills.empty());
    e.Finalize();  // close: no auction order, but the resting order must expire
    CHECK(c.fills.empty());
    CHECK(c.cancels.size() == 1 && c.cancels[0].first == "rest" && c.cancels[0].second);
  }

  // The documented A8 workaround: the same order submitted as an explicit
  // closing-auction order fills at the cross (no expiry).
  {
    Capture c;
    FillEngine e = Make(&c);
    e.AddSymbol("2330");
    e.OnTrade("2330", 10000, 10, Us(10, 0, 0), false);  // reference := 100.00
    e.Submit(Order("rest", "2330", Side::kBuy, 10000, 1000, Us(13, 25, 0)));
    e.Submit(Order("liq", "2330", Side::kSell, 10000, 5000, Us(13, 25, 30)));
    e.Finalize();
    long got = 0;
    for (auto& f : c.fills)
      if (f.id == "rest") got += f.shares;
    CHECK(got == 1000);
    CHECK(c.cancels.empty());
  }

  // Determinism: same tape twice -> byte-identical fill sequence.
  {
    Capture a = RunTape();
    Capture b = RunTape();
    CHECK(a == b);
    CHECK(!a.fills.empty());
  }

  if (g_failures == 0) {
    std::printf("test_fill_engine: OK\n");
    return 0;
  }
  std::printf("test_fill_engine: FAILED %d check(s)\n", g_failures);
  return 1;
}
