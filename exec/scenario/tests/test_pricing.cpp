// Pure-logic self-tests for limit pricing + order sizing. No broker, no network.

#include <cstdio>
#include <string>

#include "pricing.h"
#include "quote_book.h"
#include "scenario.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                                   \
  do {                                                                                   \
    auto _a = (a);                                                                       \
    auto _b = (b);                                                                       \
    if (!(_a == _b)) {                                                                   \
      std::printf("FAIL  %s:%d  %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                  (long long)_a, (long long)_b);                                         \
      ++g_failures;                                                                      \
    }                                                                                    \
  } while (0)

static TopOfBook MakeBook(Cents bid, Cents ask, Cents last) {
  TopOfBook t;
  t.best_bid = bid;
  t.best_bid_vol = bid > 0 ? 10 : 0;
  t.best_ask = ask;
  t.best_ask_vol = ask > 0 ? 10 : 0;
  t.last_trade = last;
  t.valid = true;
  return t;
}

static void TestPricing() {
  std::string reason;
  Scenario s;  // defaults: kOddLot, kBuy, kCross, kStock, two_sided, dev 9%
  auto book = MakeBook(1000'00, 1005'00, 1005'00);  // 5.00 tick grid

  CHECK_EQ(DecideLimitPrice(s, book, 0, reason), 1005'00);  // cross buy -> ask

  s.price_policy = PricePolicy::kJoin;
  CHECK_EQ(DecideLimitPrice(s, book, 0, reason), 1000'00);  // join buy -> bid (the peg)

  s.price_policy = PricePolicy::kMid;
  CHECK_EQ(DecideLimitPrice(s, MakeBook(1000'00, 1010'00, 1005'00), 0, reason), 1005'00);

  s.price_policy = PricePolicy::kCross;
  s.side = Side::kSell;
  CHECK_EQ(DecideLimitPrice(s, book, 0, reason), 1000'00);  // sell cross -> bid
  s.side = Side::kBuy;

  s.tick_offset = 1;  // +1 tick (5.00) more aggressive buy at cross
  CHECK_EQ(DecideLimitPrice(s, book, 0, reason), 1010'00);
  s.tick_offset = 0;

  CHECK_EQ(DecideLimitPrice(s, MakeBook(0, 1005'00, 0), 0, reason), 0);  // one-sided -> skip
  s.require_two_sided = false;
  CHECK_EQ(DecideLimitPrice(s, MakeBook(0, 1005'00, 0), 0, reason), 1005'00);
  s.require_two_sided = true;

  auto trial = MakeBook(1004'00, 1005'00, 1005'00);
  trial.is_trial = true;
  CHECK_EQ(DecideLimitPrice(s, trial, 0, reason), 0);  // 試撮 ignored by default

  CHECK_EQ(DecideLimitPrice(s, MakeBook(1000'00, 1005'00, 1'00), 0, reason), 0);  // dev guard
  CHECK_EQ(DecideLimitPrice(s, book, 1000'00, reason), 1005'00);                  // fixed ref ok
}

static void TestEtfPricing() {
  std::string reason;
  Scenario s;  // ETF on the 0.05 grid above 50
  s.product = Product::kEtf;
  auto book = MakeBook(190'00, 190'05, 190'00);

  CHECK_EQ(DecideLimitPrice(s, book, 0, reason), 190'05);  // cross buy -> ask (0.05 grid)
  s.price_policy = PricePolicy::kJoin;
  CHECK_EQ(DecideLimitPrice(s, book, 0, reason), 190'00);  // join buy -> bid
  s.price_policy = PricePolicy::kCross;
  s.tick_offset = 1;  // +1 ETF tick = 0.05
  CHECK_EQ(DecideLimitPrice(s, book, 0, reason), 190'10);
}

static void TestOrderSizing() {
  Scenario s;  // kOddLot, auto sizing (cap auto 1403)
  CHECK_EQ(DecideOrderShares(s, 1005'00, 300000), 1);
  CHECK_EQ(DecideOrderShares(s, 1005'00, 1005), 1);  // exactly one share affordable
  CHECK_EQ(DecideOrderShares(s, 1005'00, 1000), 0);  // can't afford one share
  CHECK_EQ(DecideOrderShares(s, 1005'00, 0), 0);     // budget done

  CHECK_EQ(DecideOrderShares(s, 50'00, 300000), 28);  // floor(1403/50)
  CHECK_EQ(DecideOrderShares(s, 50'00, 1000), 20);    // floor(1000/50) < 28

  Scenario r;
  r.board = Board::kRoundLot;
  CHECK_EQ(DecideOrderShares(r, 50'00, 300000), 1000);  // 1 lot default
  CHECK_EQ(DecideOrderShares(r, 50'00, 40000), 0);      // floor(40000/50)=800 < 1 lot
  r.shares_per_order = 2000;
  CHECK_EQ(DecideOrderShares(r, 50'00, 300000), 2000);
}

static void TestPegLevel() {
  std::string reason;
  Scenario s;
  s.price_policy = PricePolicy::kJoin;
  s.side = Side::kBuy;
  auto book = MakeBook(100'00, 100'50, 100'00);  // stock, across the 100 boundary

  s.peg_level = 1;
  CHECK_EQ(DecideLimitPrice(s, book, 0, reason), 100'00);  // best bid
  s.peg_level = 2;
  CHECK_EQ(DecideLimitPrice(s, book, 0, reason), 99'90);  // 1 tick deeper (0.10 tier)
  s.peg_level = 3;
  CHECK_EQ(DecideLimitPrice(s, book, 0, reason), 99'80);

  // peg_level applies to join only, not cross
  s.price_policy = PricePolicy::kCross;
  s.peg_level = 2;
  CHECK_EQ(DecideLimitPrice(s, book, 0, reason), 100'50);  // still best ask

  // sell join steps deeper = up
  s.price_policy = PricePolicy::kJoin;
  s.side = Side::kSell;
  s.peg_level = 2;
  CHECK_EQ(DecideLimitPrice(s, book, 0, reason), 101'00);  // ask + 1 tick (0.50)

  // composes with tick_offset
  s.side = Side::kBuy;
  s.peg_level = 2;    // -> 99.90
  s.tick_offset = 1;  // +1 tick more aggressive -> 100.00
  CHECK_EQ(DecideLimitPrice(s, book, 0, reason), 100'00);
}

int main() {
  TestPricing();
  TestEtfPricing();
  TestPegLevel();
  TestOrderSizing();
  if (g_failures == 0) {
    std::printf("test_pricing: OK\n");
    return 0;
  }
  std::printf("test_pricing: FAILED %d check(s)\n", g_failures);
  return 1;
}
