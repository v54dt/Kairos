#include "quote_encode.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

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

}  // namespace kairos::concords
