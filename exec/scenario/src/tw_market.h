#ifndef KAIROS_EXEC_TW_MARKET_H_
#define KAIROS_EXEC_TW_MARKET_H_

// Taiwan stock market microstructure helpers: the staggered tick table and
// price/tick rounding. Everything is computed in *integer cents* (price * 100)
// to avoid floating-point drift on money. TW ticks are always whole cents
// (0.01, 0.05, 0.10, 0.50, 1.00, 5.00 -> 1, 5, 10, 50, 100, 500 cents), so
// integer-cent arithmetic is exact.
//
// Tick table:
//   price <   10 -> 0.01   ( 1 cent)
//   price <   50 -> 0.05   ( 5 cents)
//   price <  100 -> 0.10   (10 cents)
//   price <  500 -> 0.50   (50 cents)
//   price < 1000 -> 1.00   (100 cents)
//   price >=1000 -> 5.00   (500 cents)

#include <cmath>
#include <cstdint>
#include <string>

namespace kairos::exec {

using Cents = std::int64_t;  // price in integer cents (NT$1.00 == 100)

constexpr Cents kMaxTwStockPriceCents = 99'999'00;  // exchange ceiling

// Tick size (in cents) that applies *at* the given price level.
inline Cents TickSizeCents(Cents price_cents) {
  if (price_cents < 10'00) return 1;      // < 10.00
  if (price_cents < 50'00) return 5;      // < 50.00
  if (price_cents < 100'00) return 10;    // < 100.00
  if (price_cents < 500'00) return 50;    // < 500.00
  if (price_cents < 1000'00) return 100;  // < 1000.00
  return 500;                             // >= 1000.00
}

inline bool TickAligned(Cents price_cents) {
  return price_cents > 0 && (price_cents % TickSizeCents(price_cents)) == 0;
}

// Round DOWN to the nearest valid tick (favourable for a passive buy / never
// overpay). Uses the tick that applies at the rounded level to stay correct
// across a tick boundary.
inline Cents RoundDownToTick(Cents price_cents) {
  if (price_cents <= 0) return 0;
  Cents tick = TickSizeCents(price_cents);
  Cents floored = (price_cents / tick) * tick;
  // Guard the boundary: if flooring crossed below a band edge, re-evaluate.
  Cents tick2 = TickSizeCents(floored);
  if (tick2 != tick) floored = (price_cents / tick2) * tick2;
  return floored;
}

// Round UP to the nearest valid tick (ensures a marketable buy clears the ask).
inline Cents RoundUpToTick(Cents price_cents) {
  if (price_cents <= 0) return 0;
  Cents tick = TickSizeCents(price_cents);
  Cents up = ((price_cents + tick - 1) / tick) * tick;
  Cents tick2 = TickSizeCents(up);
  if (tick2 != tick) up = ((price_cents + tick2 - 1) / tick2) * tick2;
  return up;
}

// Round to the NEAREST valid tick (ties go up).
inline Cents RoundNearestTick(Cents price_cents) {
  if (price_cents <= 0) return 0;
  Cents down = RoundDownToTick(price_cents);
  Cents tick = TickSizeCents(down);
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
