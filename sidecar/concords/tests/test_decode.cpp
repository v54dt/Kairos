#include <capnp/message.h>
#include <capnp/serialize.h>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "kairos.capnp.h"
#include "quote_encode.h"

namespace {

// Build a serialized Envelope.subscribe carrying the given symbols.
std::vector<std::uint8_t> EncodeSubscribe(const std::vector<std::string>& symbols) {
  capnp::MallocMessageBuilder msg;
  auto sub = msg.initRoot<Envelope>().initSubscribe();
  auto list = sub.initSymbols(static_cast<unsigned>(symbols.size()));
  for (unsigned i = 0; i < symbols.size(); ++i) {
    list.set(i, symbols[i].c_str());
  }
  auto flat = capnp::messageToFlatArray(msg);
  auto bytes = flat.asBytes();
  return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

}  // namespace

int main() {
  using kairos::concords::DecodeSubscribe;

  // a non-empty desired set round-trips, preserving order
  {
    auto bytes = EncodeSubscribe({"2330", "0050", "2317"});
    std::vector<std::string> out;
    assert(DecodeSubscribe(bytes.data(), bytes.size(), &out));
    assert((out == std::vector<std::string>{"2330", "0050", "2317"}));
  }

  // an empty desired set is still a valid subscribe (clears the output)
  {
    auto bytes = EncodeSubscribe({});
    std::vector<std::string> out{"stale"};
    assert(DecodeSubscribe(bytes.data(), bytes.size(), &out));
    assert(out.empty());
  }

  // a quote envelope is not a subscribe
  {
    kairos::concords::Quote q;
    q.symbol = "2330";
    q.exchange = kairos::concords::Exchange::kTwse;
    q.quote_ts_us = 1;
    q.last_price = 1;
    q.last_scale = 0;
    q.last_volume = 0;
    q.is_trial = false;
    auto bytes = kairos::concords::EncodeQuoteEnvelope(q);
    std::vector<std::string> out;
    assert(!DecodeSubscribe(bytes.data(), bytes.size(), &out));
  }

  // garbage bytes are rejected, not crashed on
  {
    std::vector<std::uint8_t> junk(16, 0xff);
    std::vector<std::string> out;
    assert(!DecodeSubscribe(junk.data(), junk.size(), &out));
  }

  std::cout << "test_decode: OK\n";
  return 0;
}
