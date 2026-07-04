#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

#include "quote_encode.h"

// Cross-language wire-format regression test (Track C5).
//
// Encodes the known Quote below via the SDK-free kairos_quote_encode lib and
// asserts the output equals the shared golden fixture
// schema/testdata/quote_golden_envelope.bin byte-for-byte. The same fixture is
// decoded and field-checked by the Rust test core/tests/golden_wire.rs. If the
// C++ encoder drifts from the frozen capnp wire format (schema reorder, setter
// change, serialization change), this test fails in CI.
//
// Known Quote encoded in the fixture (keep in sync with golden_wire.rs):
//   symbol      = "2330"
//   exchange    = kTwse
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

  std::vector<std::uint8_t> encoded = kairos::concords::EncodeQuoteEnvelope(q);
  std::vector<std::uint8_t> golden = ReadGolden(KAIROS_GOLDEN_PATH);

  if (encoded.size() != golden.size()) {
    std::cerr << "size mismatch: encoded=" << encoded.size() << " golden=" << golden.size() << "\n";
    assert(false && "encoded size differs from golden");
    return 1;
  }
  for (std::size_t i = 0; i < golden.size(); ++i) {
    if (encoded[i] != golden[i]) {
      std::cerr << "byte mismatch at index " << i << ": encoded=" << static_cast<int>(encoded[i])
                << " golden=" << static_cast<int>(golden[i]) << "\n";
      assert(false && "encoded bytes differ from golden");
      return 1;
    }
  }

  std::cout << "test_golden: OK\n";
  return 0;
}
