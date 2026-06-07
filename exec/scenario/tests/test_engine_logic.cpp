// Self-test for the pure re-peg decision + accounting. No broker, no socket.

#include <cstdio>

#include "engine_logic.h"

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

static TopOfBook MakeBook(Cents bid, Cents ask) {
  TopOfBook t;
  t.best_bid = bid;
  t.best_bid_vol = 10;
  t.best_ask = ask;
  t.best_ask_vol = 10;
  t.last_trade = bid;
  t.valid = true;
  return t;
}

static Scenario PegScenario() {
  Scenario s;  // join (peg) buy, oddlot, auto fee-optimal sizing
  s.price_policy = PricePolicy::kJoin;
  s.budget_twd = 300000;
  return s;
}

static void TestDecide() {
  Scenario s = PegScenario();
  auto book = MakeBook(500'00, 501'00);  // bid 500, tick 1.00

  // no resting -> place a fee-optimal slice at the bid (the peg)
  RestingOrder none;
  Action a = DecideAction(s, book, none, s.budget_twd);
  CHECK(a.kind == ActionKind::kPlace);
  CHECK_EQ(a.price, 500'00);
  CHECK_EQ(a.shares, OptimalSharesPerOrder(500'00, s.fees, true));

  // resting at the current bid -> nothing
  RestingOrder resting{true, 500'00, a.shares};
  CHECK(DecideAction(s, book, resting, s.budget_twd).kind == ActionKind::kNone);

  // bid moved up -> re-peg to the new bid, same shares
  auto moved = MakeBook(501'00, 502'00);
  Action r = DecideAction(s, moved, resting, s.budget_twd);
  CHECK(r.kind == ActionKind::kRepeg);
  CHECK_EQ(r.price, 501'00);
  CHECK_EQ(r.shares, resting.shares);

  // budget exhausted -> terminal done
  Action spent = DecideAction(s, book, none, 0);
  CHECK(spent.kind == ActionKind::kNone);
  CHECK(spent.done);

  // remaining dust (< one share at 500) -> terminal done, not a spin
  Action dust = DecideAction(s, book, none, 100);
  CHECK(dust.kind == ActionKind::kNone);
  CHECK(dust.done);

  // 試撮 quote ignored by default -> transient skip (NOT done)
  auto trial = MakeBook(500'00, 501'00);
  trial.is_trial = true;
  Action skip = DecideAction(s, trial, none, s.budget_twd);
  CHECK(skip.kind == ActionKind::kNone);
  CHECK(!skip.done);
}

static void TestAccounting() {
  Scenario s = PegScenario();
  Accounting acct;
  CHECK_EQ(acct.RemainingTwd(s), 300000);
  CHECK(!acct.BudgetReached(s));

  acct.RecordFill(s, 500'00, 3);  // 1500 twd
  CHECK_EQ(acct.filled_shares, 3);
  CHECK_EQ(acct.FilledTwd(), 1500);
  CHECK_EQ(acct.RemainingTwd(s), 298500);
  CHECK(acct.total_fee_twd >= 1);  // at least the NT$1 min

  // fill up to the budget
  s.budget_twd = 1500;
  CHECK(acct.BudgetReached(s));
  CHECK_EQ(acct.RemainingTwd(s), 0);
}

int main() {
  TestDecide();
  TestAccounting();
  if (g_failures == 0) {
    std::printf("test_engine_logic: OK\n");
    return 0;
  }
  std::printf("test_engine_logic: FAILED %d check(s)\n", g_failures);
  return 1;
}
