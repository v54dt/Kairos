#include "quote_encode.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <cstring>

#include "kairos.capnp.h"

namespace kairos::concords {
namespace {

::Exchange CapExchange(Exchange e) {
  switch (e) {
    case Exchange::kTwse:
      return ::Exchange::TWSE;
    case Exchange::kTpex:
      return ::Exchange::TPEX;
    case Exchange::kTfx:
      return ::Exchange::TFX;
    case Exchange::kOtc:
      return ::Exchange::OTC;
  }
  return ::Exchange::TWSE;
}

}  // namespace

std::vector<std::uint8_t> EncodeQuoteEnvelope(const Quote& q) {
  capnp::MallocMessageBuilder msg;
  auto quote = msg.initRoot<Envelope>().initQuote();
  quote.setSymbol(q.symbol.c_str());
  quote.setExchange(CapExchange(q.exchange));
  quote.setQuoteTsUs(q.quote_ts_us);

  auto bids = quote.initBids(static_cast<unsigned>(q.bids.size()));
  for (unsigned i = 0; i < q.bids.size(); ++i) {
    bids[i].setPriceMantissa(q.bids[i].price_mantissa);
    bids[i].setPriceScale(q.bids[i].price_scale);
    bids[i].setVolume(q.bids[i].volume);
  }
  auto asks = quote.initAsks(static_cast<unsigned>(q.asks.size()));
  for (unsigned i = 0; i < q.asks.size(); ++i) {
    asks[i].setPriceMantissa(q.asks[i].price_mantissa);
    asks[i].setPriceScale(q.asks[i].price_scale);
    asks[i].setVolume(q.asks[i].volume);
  }

  quote.setLastPrice(q.last_price);
  quote.setLastScale(q.last_scale);
  quote.setLastVolume(q.last_volume);
  quote.setIsTrial(q.is_trial);

  auto flat = capnp::messageToFlatArray(msg);
  auto bytes = flat.asBytes();
  return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

bool DecodeSubscribe(const std::uint8_t* data, std::size_t len, std::vector<std::string>* out) {
  if (data == nullptr || len == 0 || len % sizeof(capnp::word) != 0) {
    return false;
  }
  try {
    // Aeron fragments aren't guaranteed word-aligned; copy into aligned storage.
    std::vector<capnp::word> words(len / sizeof(capnp::word));
    std::memcpy(words.data(), data, len);
    capnp::FlatArrayMessageReader reader(kj::arrayPtr(words.data(), words.size()));
    auto env = reader.getRoot<Envelope>();
    if (env.which() != Envelope::SUBSCRIBE) {
      return false;
    }
    auto symbols = env.getSubscribe().getSymbols();
    out->clear();
    out->reserve(symbols.size());
    for (auto s : symbols) {
      out->emplace_back(s.cStr());
    }
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace kairos::concords
