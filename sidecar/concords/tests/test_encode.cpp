#include <capnp/serialize.h>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "kairos.capnp.h"
#include "quote_encode.h"

int main() {
  kairos::concords::Quote q;
  q.symbol = "2330";
  q.exchange = kairos::concords::Exchange::kTwse;
  q.quote_ts_us = 1700000000000000LL;
  q.bids = {{58000, 2, 100}, {57950, 2, 50}};
  q.asks = {{58100, 2, 80}};
  q.last_price = 58050;
  q.last_scale = 2;
  q.last_volume = 10;
  q.is_trial = false;

  std::vector<std::uint8_t> bytes = kairos::concords::EncodeQuoteEnvelope(q);
  assert(!bytes.empty());
  assert(bytes.size() % sizeof(capnp::word) == 0);

  auto words = kj::arrayPtr(reinterpret_cast<const capnp::word*>(bytes.data()),
                            bytes.size() / sizeof(capnp::word));
  capnp::FlatArrayMessageReader reader(words);
  auto env = reader.getRoot<Envelope>();

  assert(env.which() == Envelope::QUOTE);
  auto quote = env.getQuote();
  assert(std::string(quote.getSymbol().cStr()) == "2330");
  assert(quote.getExchange() == Exchange::TWSE);
  assert(quote.getQuoteTsUs() == 1700000000000000LL);
  assert(quote.getBids().size() == 2);
  assert(quote.getBids()[0].getPriceMantissa() == 58000);
  assert(quote.getBids()[0].getVolume() == 100);
  assert(quote.getBids()[1].getPriceMantissa() == 57950);
  assert(quote.getAsks().size() == 1);
  assert(quote.getAsks()[0].getPriceMantissa() == 58100);
  assert(quote.getLastPrice() == 58050);
  assert(quote.getLastScale() == 2);
  assert(!quote.getIsTrial());

  std::cout << "test_encode: OK\n";
  return 0;
}
