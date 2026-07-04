// Conservative (穿價才成交) fill model: marketable book-walk, resting
// strict-through fills, at-touch no-fill, SELL symmetry. Event-time only.

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
  std::vector<std::string> acks;
  std::vector<FillRec> fills;
  std::vector<std::string> cancels;
};

SymbolFillModel Make(Capture* c, FillMode mode) {
  return SymbolFillModel(
      "2330", mode,
      [c](const std::string& id, bool ok, const std::string&) {
        (void)ok;
        c->acks.push_back(id);
      },
      [c](const std::string& id, const Fill& f) { c->fills.push_back({id, f.shares, f.price}); },
      [c](const std::string& id, bool) { c->cancels.push_back(id); });
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

SimOrder Sell(const std::string& id, Cents price, long shares) {
  SimOrder o = Buy(id, price, shares);
  o.side = Side::kSell;
  return o;
}

}  // namespace

int main() {
  // Marketable-on-arrival BUY walks ask levels best->limit, one fill per level.
  {
    Capture c;
    auto m = Make(&c, FillMode::kConservative);
    m.OnBook(Book({{9990, 100}}, {{10000, 30}, {10050, 40}, {10100, 50}}), 1);
    m.Submit(Buy("b1", 10050, 100));  // crosses 100.00 (30) + 100.50 (40), rest 30 @ limit
    CHECK(c.acks.size() == 1 && c.acks[0] == "b1");
    CHECK(c.fills.size() == 2);
    CHECK(c.fills[0].shares == 30 && c.fills[0].price == 10000);
    CHECK(c.fills[1].shares == 40 && c.fills[1].price == 10050);
    CHECK(m.HasResting());  // 30 sh remain resting at 100.50
  }

  // Resting BUY fills ONLY on a trade strictly through its price; at-touch no fill.
  {
    Capture c;
    auto m = Make(&c, FillMode::kConservative);
    m.OnBook(Book({{10000, 100}}, {{10100, 100}}), 1);
    m.Submit(Buy("b1", 10000, 50));  // rests at best bid, not marketable
    CHECK(c.fills.empty());
    m.OnTrade(10000, 20, 2, false);  // at-touch (== 100.00) -> NO fill
    CHECK(c.fills.empty());
    m.OnTrade(9990, 20, 3, false);  // strictly below -> fills 20 @ 100.00
    CHECK(c.fills.size() == 1);
    CHECK(c.fills[0].id == "b1" && c.fills[0].shares == 20 && c.fills[0].price == 10000);
    m.OnTrade(9950, 100, 4, false);  // fills remaining 30 (capped at remaining)
    CHECK(c.fills.size() == 2);
    CHECK(c.fills[1].shares == 30);
    CHECK(!m.HasResting());
  }

  // Quote-through: a book whose best_ask drops strictly below the resting bid fills.
  {
    Capture c;
    auto m = Make(&c, FillMode::kConservative);
    m.OnBook(Book({{10000, 100}}, {{10100, 100}}), 1);
    m.Submit(Buy("b1", 10050, 40));  // inside spread, rests (best_ask 101 > 100.50)
    CHECK(c.fills.empty());
    m.OnBook(Book({{9950, 100}}, {{10000, 25}}), 2);  // best_ask 100.00 < 100.50 -> cross 25
    CHECK(c.fills.size() == 1);
    CHECK(c.fills[0].shares == 25 && c.fills[0].price == 10050);
  }

  // SELL symmetry: strictly-above trade fills a resting ask.
  {
    Capture c;
    auto m = Make(&c, FillMode::kConservative);
    m.OnBook(Book({{10000, 100}}, {{10100, 100}}), 1);
    m.Submit(Sell("s1", 10100, 60));  // rests at best ask
    m.OnTrade(10100, 30, 2, false);   // at-touch -> no fill
    CHECK(c.fills.empty());
    m.OnTrade(10150, 30, 3, false);  // strictly above -> fills 30 @ 101.00
    CHECK(c.fills.size() == 1 && c.fills[0].shares == 30 && c.fills[0].price == 10100);
  }

  // Marketable SELL walks bids best->limit.
  {
    Capture c;
    auto m = Make(&c, FillMode::kConservative);
    m.OnBook(Book({{10000, 30}, {9950, 40}}, {{10100, 100}}), 1);
    m.Submit(Sell("s1", 9950, 100));  // hits 100.00 (30) + 99.50 (40), rest 30
    CHECK(c.fills.size() == 2);
    CHECK(c.fills[0].shares == 30 && c.fills[0].price == 10000);
    CHECK(c.fills[1].shares == 40 && c.fills[1].price == 9950);
    CHECK(m.HasResting());
  }

  // Trial trade never fills.
  {
    Capture c;
    auto m = Make(&c, FillMode::kConservative);
    m.OnBook(Book({{10000, 100}}, {{10100, 100}}), 1);
    m.Submit(Buy("b1", 10000, 50));
    m.OnTrade(9900, 100, 2, true);  // 試撮 -> ignored
    CHECK(c.fills.empty());
  }

  // Cancel removes the resting remainder.
  {
    Capture c;
    auto m = Make(&c, FillMode::kConservative);
    m.OnBook(Book({{10000, 100}}, {{10100, 100}}), 1);
    m.Submit(Buy("b1", 10000, 50));
    CHECK(m.Cancel("b1", 2));
    CHECK(c.cancels.size() == 1 && c.cancels[0] == "b1");
    CHECK(!m.HasResting());
    CHECK(!m.Cancel("b1", 3));  // already gone
  }

  if (g_failures == 0) {
    std::printf("test_fill_model: OK\n");
    return 0;
  }
  std::printf("test_fill_model: FAILED %d check(s)\n", g_failures);
  return 1;
}
