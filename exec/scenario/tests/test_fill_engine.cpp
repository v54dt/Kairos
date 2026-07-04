// FillEngine multiplexer: odd-lot/unknown-symbol rejection, per-symbol
// isolation, partial-fill accounting, cancel semantics, and determinism (same
// event+order tape twice -> byte-identical fill sequence).

#include <cstdio>
#include <string>
#include <vector>

#include "fill_engine.h"
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
  return o;
}

FillEngine Make(Capture* c, FillMode mode = FillMode::kConservative) {
  return FillEngine(
      mode,
      [c](const std::string& id, bool ok, const std::string& e) { c->acks.push_back({id, ok, e}); },
      [c](const std::string& id, const Fill& f) { c->fills.push_back({id, f.shares, f.price}); },
      [c](const std::string& id, bool ok) { c->cancels.push_back({id, ok}); });
}

// Deterministic scripted tape: books, trades, submits, cancels replayed in order.
Capture RunTape() {
  Capture c;
  FillEngine e = Make(&c, FillMode::kProbQueue);
  e.AddSymbol("2330");
  e.AddSymbol("2317");
  e.OnBook("2330", Book({{58000, 100}}, {{58100, 100}}), 1);
  e.OnBook("2317", Book({{12000, 200}}, {{12050, 200}}), 2);
  e.Submit(Buy("a", "2330", 58000, 50));    // queue_ahead 100
  e.Submit(Buy("b", "2317", 12000, 300));   // queue_ahead 200
  e.OnTrade("2330", 58000, 120, 3, false);  // fill a: 120-100=20
  e.OnTrade("2317", 12000, 260, 4, false);  // fill b: 260-200=60
  e.OnBook("2330", Book({{58000, 0}}, {{58100, 100}}), 5);
  e.OnTrade("2330", 58000, 100, 6, false);  // fill a: remaining 30
  e.OnTrade("2317", 12000, 500, 7, false);  // fill b: remaining
  return c;
}

}  // namespace

int main() {
  // Odd-lot board is rejected with no fill.
  {
    Capture c;
    FillEngine e = Make(&c);
    e.AddSymbol("2330");
    e.OnBook("2330", Book({{10000, 100}}, {{10100, 100}}), 1);
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
    e.OnBook("2330", Book({{10000, 100}}, {{10100, 100}}), 1);
    e.OnBook("2317", Book({{20000, 100}}, {{20100, 100}}), 2);
    e.Submit(Buy("a", "2330", 10000, 50));
    e.OnTrade("2317", 9900, 100, 3, false);  // different symbol
    CHECK(c.fills.empty());
    e.OnTrade("2330", 9900, 100, 4, false);
    CHECK(c.fills.size() == 1 && c.fills[0].id == "a");
  }

  // Partial-fill accounting + completion: order removed when fully filled.
  {
    Capture c;
    FillEngine e = Make(&c);
    e.AddSymbol("2330");
    e.OnBook("2330", Book({{10000, 100}}, {{10100, 100}}), 1);
    e.Submit(Buy("a", "2330", 10000, 100));
    e.OnTrade("2330", 9900, 30, 2, false);  // partial 30
    e.OnTrade("2330", 9900, 40, 3, false);  // partial 40
    e.OnTrade("2330", 9900, 90, 4, false);  // remaining 30 (capped)
    CHECK(c.fills.size() == 3);
    CHECK(c.fills[0].shares == 30 && c.fills[1].shares == 40 && c.fills[2].shares == 30);
    // A later trade must not fill the completed order.
    e.OnTrade("2330", 9900, 100, 5, false);
    CHECK(c.fills.size() == 3);
  }

  // Cancel before any fill -> ok=true.
  {
    Capture c;
    FillEngine e = Make(&c);
    e.AddSymbol("2330");
    e.OnBook("2330", Book({{10000, 100}}, {{10100, 100}}), 1);
    e.Submit(Buy("a", "2330", 10000, 100));
    e.Cancel("a", 2);
    CHECK(c.cancels.size() == 1 && c.cancels[0].first == "a" && c.cancels[0].second);
    e.OnTrade("2330", 9900, 100, 3, false);  // no fill after cancel
    CHECK(c.fills.empty());
  }

  // Cancel after partial fill cancels the remainder; no post-cancel fill.
  {
    Capture c;
    FillEngine e = Make(&c);
    e.AddSymbol("2330");
    e.OnBook("2330", Book({{10000, 100}}, {{10100, 100}}), 1);
    e.Submit(Buy("a", "2330", 10000, 100));
    e.OnTrade("2330", 9900, 30, 2, false);  // partial 30
    e.Cancel("a", 3);
    CHECK(c.cancels.size() == 1 && c.cancels[0].second);
    e.OnTrade("2330", 9900, 100, 4, false);
    CHECK(c.fills.size() == 1);  // only the pre-cancel partial
  }

  // Cancel after full fill -> ok=false (nothing to cancel).
  {
    Capture c;
    FillEngine e = Make(&c);
    e.AddSymbol("2330");
    e.OnBook("2330", Book({{10000, 100}}, {{10100, 100}}), 1);
    e.Submit(Buy("a", "2330", 10000, 50));
    e.OnTrade("2330", 9900, 50, 2, false);
    CHECK(c.fills.size() == 1);
    e.Cancel("a", 3);
    CHECK(c.cancels.size() == 1 && !c.cancels[0].second);
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
