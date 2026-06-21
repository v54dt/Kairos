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
  if (bid > 0) {
    t.bids[0] = {bid, 10};
    t.n_bids = 1;
  }
  if (ask > 0) {
    t.asks[0] = {ask, 10};
    t.n_asks = 1;
  }
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

static void TestPacing() {
  Scenario s = PegScenario();  // pacing defaults to twap, budget 300000
  auto book = MakeBook(500'00, 501'00);
  RestingOrder none;
  long budget = s.budget_twd;

  // twap spreads budget across the window: behind schedule (filled 0, 50% in) -> place
  CHECK(DecideAction(s, book, none, budget, 0.5).kind == ActionKind::kPlace);

  // ahead of schedule (filled 60% but only 50% through the window) -> wait, not done
  Action ahead = DecideAction(s, book, none, budget * 4 / 10, 0.5);  // remaining 40% => filled 60%
  CHECK(ahead.kind == ActionKind::kNone);
  CHECK(!ahead.done);

  // at the schedule start (progress 0): place the first slice immediately, then space
  CHECK(DecideAction(s, book, none, budget, 0.0).kind == ActionKind::kPlace);

  // re-peg is never gated (chase the bid regardless of schedule)
  RestingOrder resting{true, 500'00, 1000};
  auto moved = MakeBook(501'00, 502'00);
  CHECK(DecideAction(s, moved, resting, budget, 0.0).kind == ActionKind::kRepeg);

  // asap -> no schedule gate, place to fill as fast as possible
  s.pacing = Pacing::kAsap;
  CHECK(DecideAction(s, book, none, budget, 0.0).kind == ActionKind::kPlace);
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

static void TestWindow() {
  const int start = 900, end = 1325, close = 1330;
  // before start -> wait; inside -> twap; past end pre-close -> fill remainder; close -> stop
  CHECK(ClassifyWindow(830, true, start, end, close) == WindowPhase::kWaitForOpen);
  CHECK(ClassifyWindow(1000, true, start, end, close) == WindowPhase::kInWindow);
  CHECK(ClassifyWindow(1327, true, start, end, close) == WindowPhase::kFillRemainder);
  CHECK(ClassifyWindow(1330, true, start, end, close) == WindowPhase::kClosed);
  CHECK(ClassifyWindow(1400, true, start, end, close) == WindowPhase::kClosed);

  // non-trading day: wait before close, hard-stop after (never spins all afternoon)
  CHECK(ClassifyWindow(1000, false, start, end, close) == WindowPhase::kWaitForOpen);
  CHECK(ClassifyWindow(1340, false, start, end, close) == WindowPhase::kClosed);

  // pre-market window (0050 case): not filled by 09:00 -> keep filling into the session
  CHECK(ClassifyWindow(845, true, 830, 900, close) == WindowPhase::kInWindow);
  CHECK(ClassifyWindow(1000, true, 830, 900, close) == WindowPhase::kFillRemainder);
}

int main() {
  TestDecide();
  TestPacing();
  TestWindow();
  TestAccounting();
  if (g_failures == 0) {
    std::printf("test_engine_logic: OK\n");
    return 0;
  }
  std::printf("test_engine_logic: FAILED %d check(s)\n", g_failures);
  return 1;
}
