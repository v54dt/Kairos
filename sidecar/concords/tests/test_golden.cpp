#include <capnp/serialize.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

#include "kairos.capnp.h"

// Cross-language wire-format regression test (Track C5), append-only edition.
//
// Decodes the shared pre-A2 golden fixture
// schema/testdata/quote_golden_envelope.bin (encoded before the A2 fields
// existed) and asserts every original field decodes correctly AND every A2 field
// reads back its capnp default. This is the C++ half of the proof that old
// recordings still decode after the append-only schema growth; the Rust half is
// core/tests/golden_wire.rs.
//
// (Before A2 this test byte-matched the C++ encoder output against the fixture.
// Appending fields legitimately grows the encoder output — capnp does not trim
// trailing zero words — so the encode byte-match moved to test_golden_v2.cpp,
// which pins the new wire format against a new fixture.)
//
// Known Quote encoded in the fixture (keep in sync with golden_wire.rs):
//   symbol      = "2330"
//   exchange    = TWSE
//   quote_ts_us = 1700000000000000
//   bids        = [{58000, 2, 100}, {57950, 2, 50}]
//   asks        = [{58100, 2, 80}]
//   last_price  = 58050
//   last_scale  = 2
//   last_volume = 10
//   is_trial    = false

namespace {

std::vector<std::uint8_t> ReadGolden(const char* path) {
  std::ifstream in(path, std::ios::binary);
  assert(in && "golden fixture must be readable");
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
  std::vector<std::uint8_t> golden = ReadGolden(KAIROS_GOLDEN_PATH);
  assert(golden.size() % sizeof(capnp::word) == 0);

  auto words = kj::arrayPtr(reinterpret_cast<const capnp::word*>(golden.data()),
                            golden.size() / sizeof(capnp::word));
  capnp::FlatArrayMessageReader reader(words);
  auto env = reader.getRoot<Envelope>();

  assert(env.which() == Envelope::QUOTE);
  auto q = env.getQuote();
  assert(std::string(q.getSymbol().cStr()) == "2330");
  assert(q.getExchange() == Exchange::TWSE);
  assert(q.getQuoteTsUs() == 1700000000000000LL);
  assert(q.getBids().size() == 2);
  assert(q.getBids()[0].getPriceMantissa() == 58000);
  assert(q.getBids()[0].getVolume() == 100);
  assert(q.getBids()[1].getPriceMantissa() == 57950);
  assert(q.getAsks().size() == 1);
  assert(q.getAsks()[0].getPriceMantissa() == 58100);
  assert(q.getLastPrice() == 58050);
  assert(q.getLastScale() == 2);
  assert(q.getLastVolume() == 10);
  assert(!q.getIsTrial());

  // A2 fields absent from the pre-A2 fixture must read as capnp defaults.
  assert(q.getSource() == 0);
  assert(q.getSeq() == 0);
  assert(q.getEpoch() == 0);
  assert(q.getRecvTsUs() == 0);
  assert(q.getBoard() == QuoteBoard::UNKNOWN);
  assert(q.getSession() == Session::UNKNOWN);
  assert(q.getTradingDate() == 0);
  assert(!q.getSimtrade());
  assert(q.getUnderlyingPrice() == 0);

  std::cout << "test_golden: OK\n";
  return 0;
}
