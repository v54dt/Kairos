#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kairos::concords {

enum class Exchange { kTwse, kTpex, kTfx, kOtc };

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

std::vector<std::uint8_t> EncodeQuoteEnvelope(const Quote& q);

// Decode a serialized Envelope. If it is a Subscribe, write its (possibly empty)
// symbol list to *out and return true. Returns false for any other variant or
// malformed bytes.
bool DecodeSubscribe(const std::uint8_t* data, std::size_t len, std::vector<std::string>* out);

}  // namespace kairos::concords
