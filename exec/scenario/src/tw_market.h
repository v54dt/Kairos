#ifndef KAIROS_EXEC_TW_MARKET_H_
#define KAIROS_EXEC_TW_MARKET_H_

// Taiwan stock market microstructure helpers: the per-product staggered tick
// table and price/tick rounding. Everything is computed in *integer cents*
// (price * 100) to avoid floating-point drift on money. TW ticks are always
// whole cents, so integer-cent arithmetic is exact.
//
// The tick schedule depends on the PRODUCT (TWSE 營業細則 §62, verified against
// twse.com.tw/zh/products/system/trading.html). Each column below is a distinct
// schedule; using the wrong one yields an off-grid limit price the broker rejects
// (e.g. 0050 is an ETF: at ~190 its tick is 0.05, not the stock 0.50).
//
//   price band   stock   warrant   etf    convertible-bond
//   <    5       0.01    0.01      0.01   0.05
//   5 -  10      0.01    0.05      0.01   0.05
//   10 - 50      0.05    0.10      0.01   0.05
//   50 - 100     0.10    0.50      0.05   0.05
//   100 - 150    0.50    1.00      0.05   0.05
//   150 - 500    0.50    1.00      0.05   1.00
//   500 - 1000   1.00    5.00      0.05   1.00
//   >=  1000     5.00    5.00      0.05   5.00

#include <cmath>
#include <cstdint>
#include <string>

namespace kairos::exec {

using Cents = std::int64_t;  // price in integer cents (NT$1.00 == 100)

constexpr Cents kMaxTwStockPriceCents = 99'999'00;  // exchange ceiling

// Product class selecting the applicable tick schedule. kStock also covers
// foreign stocks / TDR / closed-end funds / preferred shares (same column).
enum class Product { kStock, kEtf, kWarrant, kConvertibleBond };

// Tick size (in cents) that applies *at* the given price level for `product`.
inline Cents TickSizeCents(Cents price_cents, Product product = Product::kStock) {
  switch (product) {
    case Product::kStock:
      if (price_cents < 10'00) return 1;      // < 10.00  -> 0.01
      if (price_cents < 50'00) return 5;      // < 50.00  -> 0.05
      if (price_cents < 100'00) return 10;    // < 100.00 -> 0.10
      if (price_cents < 500'00) return 50;    // < 500.00 -> 0.50
      if (price_cents < 1000'00) return 100;  // < 1000.00-> 1.00
      return 500;                             // >= 1000  -> 5.00
    case Product::kEtf:
      return price_cents < 50'00 ? 1 : 5;  // < 50 -> 0.01, else 0.05
    case Product::kWarrant:
      if (price_cents < 5'00) return 1;      // < 5.00   -> 0.01
      if (price_cents < 10'00) return 5;     // < 10.00  -> 0.05
      if (price_cents < 50'00) return 10;    // < 50.00  -> 0.10
      if (price_cents < 100'00) return 50;   // < 100.00 -> 0.50
      if (price_cents < 500'00) return 100;  // < 500.00 -> 1.00
      return 500;                            // >= 500   -> 5.00
    case Product::kConvertibleBond:
      if (price_cents < 150'00) return 5;     // < 150.00 -> 0.05
      if (price_cents < 1000'00) return 100;  // < 1000.00-> 1.00
      return 500;                             // >= 1000  -> 5.00
  }
  return 500;  // unreachable
}

inline bool TickAligned(Cents price_cents, Product product = Product::kStock) {
  return price_cents > 0 && (price_cents % TickSizeCents(price_cents, product)) == 0;
}

// Round DOWN to the nearest valid tick (favourable for a passive buy / never
// overpay). Uses the tick that applies at the rounded level to stay correct
// across a tick boundary.
inline Cents RoundDownToTick(Cents price_cents, Product product = Product::kStock) {
  if (price_cents <= 0) return 0;
  Cents tick = TickSizeCents(price_cents, product);
  Cents floored = (price_cents / tick) * tick;
  Cents tick2 = TickSizeCents(floored, product);
  if (tick2 != tick) floored = (price_cents / tick2) * tick2;
  return floored;
}

// Round UP to the nearest valid tick (ensures a marketable buy clears the ask).
inline Cents RoundUpToTick(Cents price_cents, Product product = Product::kStock) {
  if (price_cents <= 0) return 0;
  Cents tick = TickSizeCents(price_cents, product);
  Cents up = ((price_cents + tick - 1) / tick) * tick;
  Cents tick2 = TickSizeCents(up, product);
  if (tick2 != tick) up = ((price_cents + tick2 - 1) / tick2) * tick2;
  return up;
}

// Round to the NEAREST valid tick (ties go up).
inline Cents RoundNearestTick(Cents price_cents, Product product = Product::kStock) {
  if (price_cents <= 0) return 0;
  Cents down = RoundDownToTick(price_cents, product);
  Cents tick = TickSizeCents(down, product);
  Cents up = down + tick;
  return (price_cents - down) < (up - price_cents) ? down : up;
}

// Convert a float price (e.g. from a quote) to cents, rounding to the nearest
// cent first to absorb the digits/precision representation.
inline Cents FloatToCents(double price) { return static_cast<Cents>(std::llround(price * 100.0)); }

inline double CentsToDouble(Cents c) { return static_cast<double>(c) / 100.0; }

// Format cents as a "X.XX" decimal string for the broker SDK.
inline std::string CentsToString(Cents c) {
  Cents whole = c / 100;
  Cents frac = c % 100;
  if (frac < 0) frac = -frac;
  std::string s = std::to_string(whole);
  s += '.';
  if (frac < 10) s += '0';
  s += std::to_string(frac);
  return s;
}

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_TW_MARKET_H_
