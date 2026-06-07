#ifndef KAIROS_EXEC_TW_FEES_H_
#define KAIROS_EXEC_TW_FEES_H_

// Taiwan brokerage fee model + optimal odd-lot order sizing.
//
// Fee per execution = trunc(trade_value * brokerage_rate), floored at a broker
// minimum. The brokerage_rate is the statutory 0.1425% times an electronic
// discount multiplier (e.g. 0.65 == 65 折). Taiwan truncates (無條件捨去) the
// computed fee to whole NT$.
//
// "Optimal fee" for odd-lot accumulation: keep each order's notional at or
// below the largest value N where trunc(N * rate) <= min_fee. Below that the
// fee is pinned at the minimum (typically NT$1), so splitting a large buy into
// many <=N-dollar slices never pays more than the minimum per slice.
//
//   With rate = 0.1425% (no discount) and min_fee = 1:
//     trunc(N * 0.001425) <= 1  <=>  N < 2/0.001425 = 1403.5  =>  N_opt = 1403

#include <algorithm>
#include <cmath>

#include "tw_market.h"

namespace kairos::exec {

struct FeeParams {
  double base_rate = 0.001425;        // statutory brokerage rate (0.1425%)
  double discount = 1.0;              // electronic discount multiplier (<=1.0)
  long min_fee_oddlot = 1;            // NT$ min fee per odd-lot execution
  long min_fee_roundlot = 20;         // NT$ min fee per round-lot execution
  double sell_tax_rate = 0.003;       // 證交稅, normal sell
  double daytrade_tax_rate = 0.0015;  // 當沖證交稅
  // Explicit per-order notional cap (NT$). 0 => auto-derive the fee-optimal cap.
  long max_order_value_twd = 0;
};

inline double EffectiveRate(const FeeParams& p) { return p.base_rate * p.discount; }

inline long MinFee(const FeeParams& p, bool is_oddlot) {
  return is_oddlot ? p.min_fee_oddlot : p.min_fee_roundlot;
}

// Brokerage fee (NT$) for a buy/sell of the given notional (in cents).
inline long BrokerageFee(Cents notional_cents, const FeeParams& p, bool is_oddlot) {
  double value = CentsToDouble(notional_cents);
  long raw = static_cast<long>(std::floor(value * EffectiveRate(p)));  // 無條件捨去
  return std::max(raw, MinFee(p, is_oddlot));
}

// Securities transaction tax (NT$) on a SELL of the given notional.
inline long SellTax(Cents notional_cents, const FeeParams& p, bool daytrade) {
  double value = CentsToDouble(notional_cents);
  double rate = daytrade ? p.daytrade_tax_rate : p.sell_tax_rate;
  return static_cast<long>(std::floor(value * rate));
}

// Largest per-order notional (NT$) whose brokerage fee is still the minimum.
inline long OptimalMaxOrderValueTwd(const FeeParams& p, bool is_oddlot) {
  double rate = EffectiveRate(p);
  if (rate <= 0.0) return kMaxTwStockPriceCents / 100;
  long min_fee = MinFee(p, is_oddlot);
  // Largest integer N with floor(N*rate) <= min_fee  <=>  N < (min_fee+1)/rate.
  long n = static_cast<long>(std::ceil((static_cast<double>(min_fee) + 1.0) / rate)) - 1;
  // Defensive: walk back until the invariant holds (guards rounding at the edge).
  while (n > 1 && static_cast<long>(std::floor(static_cast<double>(n) * rate)) > min_fee) {
    --n;
  }
  return std::max<long>(n, 1);
}

// Resolved per-order notional cap: the explicit override if set, else optimal.
inline long ResolvedMaxOrderValueTwd(const FeeParams& p, bool is_oddlot) {
  return p.max_order_value_twd > 0 ? p.max_order_value_twd : OptimalMaxOrderValueTwd(p, is_oddlot);
}

// Optimal share count per order: as many shares as fit under the notional cap,
// but never fewer than 1 (a single share above the cap is unavoidable for very
// high-priced names and the caller should be warned).
inline long OptimalSharesPerOrder(Cents price_cents, const FeeParams& p, bool is_oddlot) {
  if (price_cents <= 0) return 1;
  long cap_twd = ResolvedMaxOrderValueTwd(p, is_oddlot);
  long shares = static_cast<long>(
      std::floor((static_cast<double>(cap_twd) * 100.0) / static_cast<double>(price_cents)));
  return std::max<long>(shares, 1);
}

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_TW_FEES_H_
