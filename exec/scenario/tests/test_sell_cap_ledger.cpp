// Self-test for SellCapLedger: the B4 oversell orderings. filled + still-live
// shares must never exceed the held position.

#include <cstdio>

#include "sell_cap_ledger.h"
#include "test_check.h"

using namespace kairos::exec;

namespace {

// An acked order that timed out (dropped ack) keeps its unfilled shares committed
// against the cap even after the engine stops tracking it as resting.
void TestDroppedAckCounted() {
  SellCapLedger l;
  l.AbandonResting(Side::kSell, /*active=*/true, /*shares=*/300, /*filled=*/0, "o1");
  CHECK_EQ(l.SellCapRemaining(1000, 0, false, 0, 0), 700);
  // Only the unfilled remainder of a partially-filled order carries over.
  SellCapLedger l2;
  l2.AbandonResting(Side::kSell, true, 300, 100, "o1");
  CHECK_EQ(l2.SellCapRemaining(1000, 100, false, 0, 0), 700);
}

// A rejected re-peg cancel abandons the order (it may still be live); an unrelated
// cancel confirmation must not release it.
void TestCancelRejectedRetained() {
  SellCapLedger l;
  l.AbandonResting(Side::kSell, true, 300, 0, "o1");
  CHECK(!l.ReleaseOnCancel("other"));
  CHECK_EQ(l.SellCapRemaining(1000, 0, false, 0, 0), 700);
}

// A confirmed cancel of the abandoned id releases its shares (idempotently).
void TestConfirmedCancelReleased() {
  SellCapLedger l;
  l.AbandonResting(Side::kSell, true, 300, 0, "o1");
  CHECK(l.ReleaseOnCancel("o1"));
  CHECK(!l.ReleaseOnCancel("o1"));
  CHECK_EQ(l.SellCapRemaining(1000, 0, false, 0, 0), 1000);
}

// A late fill of an abandoned order decrements its inflight and drops it once
// fully resolved; an untracked id is a no-op (never goes negative).
void TestLateFillDecrement() {
  SellCapLedger l;
  l.AbandonResting(Side::kSell, true, 300, 0, "o1");
  l.OnLateFill("o1", 100);
  CHECK_EQ(l.SellCapRemaining(1000, 0, false, 0, 0), 800);
  l.OnLateFill("o1", 200);
  CHECK_EQ(l.SellCapRemaining(1000, 0, false, 0, 0), 1000);
  l.OnLateFill("unknown", 50);
  CHECK_EQ(l.SellCapRemaining(1000, 0, false, 0, 0), 1000);
}

// Buy side never feeds the sell cap.
void TestBuySideNoOp() {
  SellCapLedger l;
  l.AbandonResting(Side::kBuy, true, 300, 0, "o1");
  CHECK_EQ(l.SellCapRemaining(1000, 0, false, 0, 0), 1000);
}

// committed = filled + resting-unfilled + inflight; clamps at zero, and surfaces a
// non-lot remainder exactly (the RoundLot-DOWN of it is DecideAction's job).
void TestCommittedMathClampAndRemainder() {
  SellCapLedger l;
  CHECK_EQ(l.SellCapRemaining(1000, 100, true, 250, 50), 700);
  l.AbandonResting(Side::kSell, true, 900, 0, "o1");
  CHECK_EQ(l.SellCapRemaining(1000, 100, true, 250, 50), 0);
  SellCapLedger l2;
  CHECK_EQ(l2.SellCapRemaining(1000, 0, true, 850, 0), 150);
}

}  // namespace

int main() {
  TestDroppedAckCounted();
  TestCancelRejectedRetained();
  TestConfirmedCancelReleased();
  TestLateFillDecrement();
  TestBuySideNoOp();
  TestCommittedMathClampAndRemainder();
  if (g_failures == 0) {
    std::printf("test_sell_cap_ledger: OK\n");
    return 0;
  }
  std::printf("test_sell_cap_ledger: FAILED %d check(s)\n", g_failures);
  return 1;
}
