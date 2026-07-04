#ifndef KAIROS_EXEC_QUOTE_CODEC_H_
#define KAIROS_EXEC_QUOTE_CODEC_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "quote_book.h"

namespace kairos::exec {

// mantissa * 10^(2 - scale): the wire's mantissa+scale -> integer cents.
Cents MantissaScaleToCents(std::int64_t mantissa, std::uint8_t scale);

// Serialized Envelope.subscribe carrying `symbols`.
std::vector<std::uint8_t> EncodeSubscribe(const std::vector<std::string>& symbols);

// Decode a serialized Envelope. If it is a Quote, fill *tob (best bid/ask/last,
// 試撮 flag, recv_ts = now) and *symbol, then return true. Returns false for any
// other variant (e.g. a Trade or control frame) or malformed bytes; a
// well-formed but unhandled variant is counted (see UnknownVariantCount).
bool DecodeQuote(const std::uint8_t* data, std::size_t len, TopOfBook* tob, std::string* symbol);

// Decode a serialized Envelope. If it is a Trade, fill *trade (price/volume/ts/
// 試撮) and *symbol, then return true. Returns false for any other variant or
// malformed bytes. Unlike DecodeQuote it does NOT count unknown variants: the sim
// hub routes Trade frames here while the counter stays a pure quote-path metric.
bool DecodeTrade(const std::uint8_t* data, std::size_t len, Trade* trade, std::string* symbol);

// Cumulative count of well-formed Envelopes whose variant DecodeQuote does not
// handle (Trade/subAck/error/heartbeat/...). Distinct from malformed bytes, which
// are not counted here. Exposed for observability and tests.
std::uint64_t UnknownVariantCount();

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_QUOTE_CODEC_H_
