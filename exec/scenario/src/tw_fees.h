#ifndef KAIROS_EXEC_TW_FEES_H_
#define KAIROS_EXEC_TW_FEES_H_

// TW brokerage fee model + fee-optimal odd-lot order sizing.

#include <algorithm>
#include <cmath>

#include "tw_market.h"

namespace kairos::exec {

struct FeeParams {
  double base_rate = 0.001425;
  double discount = 1.0;
  long min_fee_oddlot = 1;
  long min_fee_roundlot = 20;
  double sell_tax_rate = 0.003;
  double daytrade_tax_rate = 0.0015;
  long max_order_value_twd = 0;  // 0 => auto fee-optimal cap
};

inline double EffectiveRate(const FeeParams& p) { return p.base_rate * p.discount; }

inline long MinFee(const FeeParams& p, bool is_oddlot) {
  return is_oddlot ? p.min_fee_oddlot : p.min_fee_roundlot;
}

inline long BrokerageFee(Cents notional_cents, const FeeParams& p, bool is_oddlot) {
  double value = CentsToDouble(notional_cents);
  long raw = static_cast<long>(std::floor(value * EffectiveRate(p)));  // 無條件捨去
  return std::max(raw, MinFee(p, is_oddlot));
}

inline long SellTax(Cents notional_cents, const FeeParams& p, bool daytrade) {
  double value = CentsToDouble(notional_cents);
  double rate = daytrade ? p.daytrade_tax_rate : p.sell_tax_rate;
  return static_cast<long>(std::floor(value * rate));
}

// Largest notional N with floor(N*rate) <= min_fee, i.e. N < (min_fee+1)/rate.
inline long OptimalMaxOrderValueTwd(const FeeParams& p, bool is_oddlot) {
  double rate = EffectiveRate(p);
  if (rate <= 0.0) return kMaxTwStockPriceCents / 100;
  long min_fee = MinFee(p, is_oddlot);
  long n = static_cast<long>(std::ceil((static_cast<double>(min_fee) + 1.0) / rate)) - 1;
  while (n > 1 && static_cast<long>(std::floor(static_cast<double>(n) * rate)) > min_fee) {
    --n;
  }
  return std::max<long>(n, 1);
}

inline long ResolvedMaxOrderValueTwd(const FeeParams& p, bool is_oddlot) {
  return p.max_order_value_twd > 0 ? p.max_order_value_twd : OptimalMaxOrderValueTwd(p, is_oddlot);
}

inline long OptimalSharesPerOrder(Cents price_cents, const FeeParams& p, bool is_oddlot) {
  if (price_cents <= 0) return 1;
  long cap_twd = ResolvedMaxOrderValueTwd(p, is_oddlot);
  long shares = static_cast<long>(
      std::floor((static_cast<double>(cap_twd) * 100.0) / static_cast<double>(price_cents)));
  return std::max<long>(shares, 1);
}

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_TW_FEES_H_
