#ifndef KAIROS_EXEC_ENGINE_LOGIC_H_
#define KAIROS_EXEC_ENGINE_LOGIC_H_

// Pure (SDK-free) re-peg decision + accounting. The runtime drives the SDK and
// timing; this file holds the money logic so it stays unit-testable.

#include <string>

#include "pricing.h"
#include "quote_book.h"
#include "scenario.h"
#include "tw_fees.h"
#include "tw_market.h"

namespace kairos::exec {

enum class ActionKind { kNone, kPlace, kRepeg };

struct Action {
  ActionKind kind = ActionKind::kNone;
  Cents price = 0;
  long shares = 0;
  bool done = false;   // terminal: budget met or remaining can't buy another slice
  std::string reason;  // why kNone (skip / done)
};

struct RestingOrder {
  bool active = false;
  Cents price = 0;
  long shares = 0;
};

// Quantity is the fee-optimal slice (only the PRICE is dynamic). With no resting
// order, place a slice at the target; with one resting, re-peg its price when the
// target tick moves; otherwise do nothing.
//
// window_progress: linear position through [window_start, window_end], 0..1. twap
// spreads the budget evenly across the window: only place while filled is behind
// the schedule (budget * progress), wait once on/ahead of it. Re-peg of the
// current slice is never gated (the scenario owns the cadence, the hub is a gateway).
inline Action DecideAction(const Scenario& s, const TopOfBook& tob, const RestingOrder& resting,
                           long remaining_twd, double window_progress = 1.0) {
  Action a;
  if (remaining_twd <= 0) {
    a.reason = "budget reached";
    a.done = true;
    return a;
  }
  std::string reason;
  Cents target = DecideLimitPrice(s, tob, FloatToCents(s.reference_price), reason);
  if (target <= 0) {
    a.reason = reason;  // transient skip (試撮 / one-sided / deviation)
    return a;
  }
  if (!resting.active) {
    if (s.pacing == Pacing::kTwap) {
      long filled = s.budget_twd - remaining_twd;
      long scheduled = static_cast<long>(s.budget_twd * window_progress);
      if (filled > scheduled) {
        a.reason = "twap: ahead of schedule";  // wait for the schedule to advance
        return a;
      }
    }
    long shares = DecideOrderShares(s, target, remaining_twd);
    if (shares <= 0) {
      a.reason = "remaining below one slice";
      a.done = true;  // dust: can't afford another share at this price
      return a;
    }
    a.kind = ActionKind::kPlace;
    a.price = target;
    a.shares = shares;
    return a;
  }
  if (resting.price != target) {
    a.kind = ActionKind::kRepeg;
    a.price = target;
    a.shares = resting.shares;
  }
  return a;
}

// Filled accounting + budget gate. Placement only happens when nothing rests, so
// remaining = budget - filled is sufficient (no double counting).
struct Accounting {
  long filled_shares = 0;
  Cents filled_notional = 0;
  long total_fee_twd = 0;

  void RecordFill(const Scenario& s, Cents price, long shares) {
    filled_shares += shares;
    filled_notional += price * shares;
    total_fee_twd += BrokerageFee(price * shares, s.fees, s.IsOddLot());
  }

  long FilledTwd() const { return filled_notional / 100; }
  long RemainingTwd(const Scenario& s) const {
    long r = s.budget_twd - FilledTwd();
    return r > 0 ? r : 0;
  }
  bool BudgetReached(const Scenario& s) const { return RemainingTwd(s) <= 0; }
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ENGINE_LOGIC_H_
