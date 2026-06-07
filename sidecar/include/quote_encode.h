#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kairos {

enum class Exchange { Twse, Tpex, Tfx, Otc };

struct Level {
  std::int64_t price_mantissa;
  std::uint8_t price_scale;
  std::int64_t volume;
};

struct Quote {
  std::string symbol;
  Exchange exchange;
  std::int64_t quote_ts_us;
  std::vector<Level> bids;
  std::vector<Level> asks;
  std::int64_t last_price;
  std::uint8_t last_scale;
  std::int64_t last_volume;
  bool is_trial;
};

std::vector<std::uint8_t> encode_quote_envelope(const Quote& q);

}  // namespace kairos
