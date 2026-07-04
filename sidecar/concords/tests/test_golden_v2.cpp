#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

#include "quote_encode.h"

// Cross-language regression for the A2 Quote wire format: encodes the known Quote
// below (with the new source/seq/epoch/recvTsUs/board/session fields set) via the
// SDK-free encode lib and byte-matches schema/testdata/quote_v2_golden_envelope.bin,
// which the Rust test core/tests/golden_wire_v2.rs also decodes and field-checks.
//
// Known Quote (keep in sync with golden_wire_v2.rs):
//   symbol=2330 exchange=kTwse quote_ts_us=1700000000000000
//   bids=[{58000,2,100},{57950,2,50}] asks=[{58100,2,80}]
//   last_price=58050 last_scale=2 last_volume=10 is_trial=false
//   source=0 seq=12345 epoch=7 recv_ts_us=1700000000000042
//   board=kRoundLot session=kDay trading_date=20260704 simtrade=false underlying_price=0

namespace {

std::vector<std::uint8_t> ReadGolden(const char* path) {
  std::ifstream in(path, std::ios::binary);
  assert(in && "v2 golden fixture must be readable");
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
  q.source = 0;
  q.seq = 12345;
  q.epoch = 7;
  q.recv_ts_us = 1700000000000042LL;
  q.board = kairos::concords::QuoteBoard::kRoundLot;
  q.session = kairos::concords::Session::kDay;
  q.trading_date = 20260704;
  q.simtrade = false;
  q.underlying_price = 0;

  std::vector<std::uint8_t> encoded = kairos::concords::EncodeQuoteEnvelope(q);
  std::vector<std::uint8_t> golden = ReadGolden(KAIROS_QUOTE_V2_GOLDEN_PATH);

  if (encoded.size() != golden.size()) {
    std::cerr << "size mismatch: encoded=" << encoded.size() << " golden=" << golden.size() << "\n";
    assert(false && "encoded size differs from v2 golden");
    return 1;
  }
  for (std::size_t i = 0; i < golden.size(); ++i) {
    if (encoded[i] != golden[i]) {
      std::cerr << "byte mismatch at index " << i << ": encoded=" << static_cast<int>(encoded[i])
                << " golden=" << static_cast<int>(golden[i]) << "\n";
      assert(false && "encoded bytes differ from v2 golden");
      return 1;
    }
  }

  std::cout << "test_golden_v2: OK\n";
  return 0;
}
