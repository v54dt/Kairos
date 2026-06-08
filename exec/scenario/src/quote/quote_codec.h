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
// 試撮 flag, recv_ts = now) and *symbol, then return true. False otherwise.
bool DecodeQuote(const std::uint8_t* data, std::size_t len, TopOfBook* tob, std::string* symbol);

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_QUOTE_CODEC_H_
