#ifndef KAIROS_EXEC_PRICING_H_
#define KAIROS_EXEC_PRICING_H_

// Pure (SDK-free) limit-price + order-size decisions. The engine re-pegs by
// re-evaluating DecideLimitPrice against the live quote.

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

#include "quote_book.h"
#include "scenario.h"
#include "tw_fees.h"
#include "tw_market.h"

namespace kairos::exec {

// Limit price (cents) from the snapshot, or 0 + reason when the engine should
// skip. fixed_reference (cents) is the configured deviation band anchor, or 0 to
// use the live last/mid.
inline Cents DecideLimitPrice(const Scenario& s, const TopOfBook& tob, Cents fixed_reference,
                              std::string& reason) {
  if (tob.is_trial && !s.use_trial_quotes) {
    reason = "trial quote, skipped";
    return 0;
  }
  if (s.require_two_sided && !tob.HasTwoSided()) {
    reason = "not two-sided";
    return 0;
  }

  const bool buy = s.side == Side::kBuy;
  Cents base = 0;
  switch (s.price_policy) {
    case PricePolicy::kCross:
      base = buy ? tob.best_ask : tob.best_bid;
      break;
    case PricePolicy::kJoin:
      base = buy ? tob.best_bid : tob.best_ask;
      break;
    case PricePolicy::kMid:
      base = tob.Mid();
      break;
    case PricePolicy::kLast:
      base = tob.last_trade;
      break;
  }
  if (base <= 0) {
    reason = "no price for policy";
    return 0;
  }

  if (s.tick_offset != 0) {
    Cents tick = TickSizeCents(base, s.product);
    base += static_cast<Cents>(s.tick_offset) * tick * (buy ? 1 : -1);
  }
  Cents price = RoundNearestTick(base, s.product);
  if (price <= 0) {
    reason = "invalid price";
    return 0;
  }

  Cents ref =
      fixed_reference > 0 ? fixed_reference : (tob.last_trade > 0 ? tob.last_trade : tob.Mid());
  if (ref > 0) {
    double dev = std::fabs(static_cast<double>(price - ref)) / static_cast<double>(ref) * 100.0;
    if (dev > s.max_deviation_pct) {
      reason = std::format("price {} deviates from ref {} by {:.2f}% > {:.2f}%",
                           CentsToString(price), CentsToString(ref), dev, s.max_deviation_pct);
      return 0;
    }
  }
  return price;
}

// Shares for one order at price_cents, capped by remaining budget. 0 => place no
// further order. Round-lot results are whole 1000-share lots.
inline long DecideOrderShares(const Scenario& s, Cents price_cents, long remaining_twd) {
  if (price_cents <= 0 || remaining_twd <= 0) return 0;
  const bool oddlot = s.IsOddLot();

  long base_shares;
  if (s.shares_per_order > 0) {
    base_shares = s.shares_per_order;
  } else if (oddlot) {
    base_shares = OptimalSharesPerOrder(price_cents, s.fees, true);
  } else {
    base_shares = 1000;
  }

  long max_affordable = static_cast<long>(
      std::floor((static_cast<double>(remaining_twd) * 100.0) / static_cast<double>(price_cents)));
  if (max_affordable <= 0) return 0;

  long shares = std::min(base_shares, max_affordable);
  if (!oddlot) {
    shares = (shares / 1000) * 1000;
    if (shares == 0) return 0;
  }
  return std::max<long>(shares, 0);
}

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_PRICING_H_
