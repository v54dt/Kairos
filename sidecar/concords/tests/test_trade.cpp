#include <capnp/serialize.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "kairos.capnp.h"
#include "quote_encode.h"

// Cross-language regression for the A2 Trade wire format: encodes the known Trade
// below via the SDK-free encode lib and byte-matches the shared fixture
// schema/testdata/trade_golden_envelope.bin, which the Rust test
// core/tests/golden_wire_v2.rs also decodes and field-checks. Then round-trips
// the bytes back through the capnp reader.
//
// Known Trade (keep in sync with golden_wire_v2.rs):
//   symbol=2330 exchange=kTwse source=0 seq=12346 epoch=7
//   trade_ts_us=1700000000000100 recv_ts_us=1700000000000142
//   price_mantissa=58050 price_scale=2 volume=3 is_trial=false
//   session=kDay trading_date=20260704 simtrade=false underlying_price=0

namespace {

std::vector<std::uint8_t> ReadGolden(const char* path) {
  std::ifstream in(path, std::ios::binary);
  assert(in && "trade golden fixture must be readable");
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
  kairos::concords::Trade t;
  t.symbol = "2330";
  t.exchange = kairos::concords::Exchange::kTwse;
  t.source = 0;
  t.seq = 12346;
  t.epoch = 7;
  t.trade_ts_us = 1700000000000100LL;
  t.recv_ts_us = 1700000000000142LL;
  t.price_mantissa = 58050;
  t.price_scale = 2;
  t.volume = 3;
  t.is_trial = false;
  t.session = kairos::concords::Session::kDay;
  t.trading_date = 20260704;
  t.simtrade = false;
  t.underlying_price = 0;

  std::vector<std::uint8_t> encoded = kairos::concords::EncodeTradeEnvelope(t);
  std::vector<std::uint8_t> golden = ReadGolden(KAIROS_TRADE_GOLDEN_PATH);
  if (encoded != golden) {
    std::cerr << "trade encode differs from golden: encoded=" << encoded.size()
              << " golden=" << golden.size() << "\n";
    assert(false && "trade encode bytes differ from golden");
    return 1;
  }

  auto words = kj::arrayPtr(reinterpret_cast<const capnp::word*>(encoded.data()),
                            encoded.size() / sizeof(capnp::word));
  capnp::FlatArrayMessageReader reader(words);
  auto env = reader.getRoot<Envelope>();
  assert(env.which() == Envelope::TRADE);
  auto tr = env.getTrade();
  assert(std::string(tr.getSymbol().cStr()) == "2330");
  assert(tr.getSeq() == 12346);
  assert(tr.getEpoch() == 7);
  assert(tr.getPriceMantissa() == 58050);
  assert(tr.getVolume() == 3);
  assert(tr.getSession() == Session::DAY);

  std::cout << "test_trade: OK\n";
  return 0;
}
