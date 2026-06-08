#ifndef KAIROS_EXEC_TW_MARKET_H_
#define KAIROS_EXEC_TW_MARKET_H_

// Per-product TW tick table (TWSE 營業細則 §62) + integer-cent price rounding.

#include <cmath>
#include <cstdint>
#include <string>

namespace kairos::exec {

using Cents = std::int64_t;  // NT$1.00 == 100

constexpr Cents kMaxTwStockPriceCents = 99'999'00;

// kStock covers foreign stocks / TDR / closed-end funds / preferred shares.
// NOTE: not auto-detected — concords exposes no security type, so the caller
// (scenario config) must supply it.
enum class Product { kStock, kEtf, kWarrant, kConvertibleBond };

inline Cents TickSizeCents(Cents price_cents, Product product = Product::kStock) {
  switch (product) {
    case Product::kStock:
      if (price_cents < 10'00) return 1;
      if (price_cents < 50'00) return 5;
      if (price_cents < 100'00) return 10;
      if (price_cents < 500'00) return 50;
      if (price_cents < 1000'00) return 100;
      return 500;
    case Product::kEtf:
      return price_cents < 50'00 ? 1 : 5;
    case Product::kWarrant:
      if (price_cents < 5'00) return 1;
      if (price_cents < 10'00) return 5;
      if (price_cents < 50'00) return 10;
      if (price_cents < 100'00) return 50;
      if (price_cents < 500'00) return 100;
      return 500;
    case Product::kConvertibleBond:
      if (price_cents < 150'00) return 5;
      if (price_cents < 1000'00) return 100;
      return 500;
  }
  return 500;
}

inline bool TickAligned(Cents price_cents, Product product = Product::kStock) {
  return price_cents > 0 && (price_cents % TickSizeCents(price_cents, product)) == 0;
}

inline Cents RoundDownToTick(Cents price_cents, Product product = Product::kStock) {
  if (price_cents <= 0) return 0;
  Cents tick = TickSizeCents(price_cents, product);
  Cents floored = (price_cents / tick) * tick;
  Cents tick2 = TickSizeCents(floored, product);
  if (tick2 != tick) floored = (price_cents / tick2) * tick2;
  return floored;
}

inline Cents RoundUpToTick(Cents price_cents, Product product = Product::kStock) {
  if (price_cents <= 0) return 0;
  Cents tick = TickSizeCents(price_cents, product);
  Cents up = ((price_cents + tick - 1) / tick) * tick;
  Cents tick2 = TickSizeCents(up, product);
  if (tick2 != tick) up = ((price_cents + tick2 - 1) / tick2) * tick2;
  return up;
}

inline Cents RoundNearestTick(Cents price_cents, Product product = Product::kStock) {
  if (price_cents <= 0) return 0;
  Cents down = RoundDownToTick(price_cents, product);
  Cents tick = TickSizeCents(down, product);
  Cents up = down + tick;
  return (price_cents - down) < (up - price_cents) ? down : up;
}

inline Cents FloatToCents(double price) { return static_cast<Cents>(std::llround(price * 100.0)); }

inline double CentsToDouble(Cents c) { return static_cast<double>(c) / 100.0; }

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
