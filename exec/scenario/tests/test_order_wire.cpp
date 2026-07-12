#include <capnp/serialize.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "order_codec.h"
#include "test_check.h"

// Single-language wire golden (Track RF1 E): schema/testdata/order_golden.bin pins
// the OrderEnvelope wire format for every variant (submit / cancel / ack / fill /
// cancel_result) produced by the C++ order_codec. No Rust consumer exists yet, so a
// one-sided pin suffices; this catches encoder drift (schema reorder, field-mapping
// change) exactly as the quote/trade wire goldens do. The file is a sequence of
// length-prefixed frames: uint32 little-endian length, then that many bytes.
// capnp encoding is deterministic (no clock), so the whole file is byte-compared and
// every frame is decoded back and asserted.
//
// Regeneration: KAIROS_REGEN=1 ctest -R test_order_wire.

namespace {

using namespace kairos::exec;

void PutFrame(std::string* out, const std::vector<std::uint8_t>& b) {
  std::uint32_t n = static_cast<std::uint32_t>(b.size());
  out->push_back(static_cast<char>(n & 0xFF));
  out->push_back(static_cast<char>((n >> 8) & 0xFF));
  out->push_back(static_cast<char>((n >> 16) & 0xFF));
  out->push_back(static_cast<char>((n >> 24) & 0xFF));
  out->append(reinterpret_cast<const char*>(b.data()), b.size());
}

OrderSubmitMsg Submit() {
  OrderSubmitMsg m;
  m.id = "k12345-7";
  m.symbol = "2330";
  m.market = Market::kOtc;
  m.board = Board::kRoundLot;
  m.side = Side::kSell;
  m.funding_type = "Cash";
  m.time_in_force = "ROD";
  m.price = 92500;
  m.shares = 2000;
  return m;
}

std::string BuildFile() {
  std::string out;
  PutFrame(&out, EncodeOrderSubmit(Submit()));
  PutFrame(&out, EncodeOrderCancel({"k12345-7"}));
  PutFrame(&out, EncodeOrderAck({"k12345-7", false, "rejected by broker"}));
  PutFrame(&out, EncodeOrderFill({"k12345-7", 1000, 92500}));
  PutFrame(&out, EncodeOrderCancelResult({"k12345-7", true}));
  return out;
}

std::vector<std::vector<std::uint8_t>> Frames(const std::string& blob) {
  std::vector<std::vector<std::uint8_t>> out;
  std::size_t i = 0;
  while (i + 4 <= blob.size()) {
    std::uint32_t n = static_cast<std::uint8_t>(blob[i]) |
                      (static_cast<std::uint8_t>(blob[i + 1]) << 8) |
                      (static_cast<std::uint8_t>(blob[i + 2]) << 16) |
                      (static_cast<std::uint8_t>(blob[i + 3]) << 24);
    i += 4;
    if (i + n > blob.size()) break;
    out.emplace_back(blob.begin() + i, blob.begin() + i + n);
    i += n;
  }
  return out;
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
  const char* path = KAIROS_ORDER_WIRE_PATH;
  std::string expected = BuildFile();

  if (const char* regen = std::getenv("KAIROS_REGEN"); regen != nullptr && regen[0] != '\0') {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    CHECK(out.good());
    out.write(expected.data(), static_cast<std::streamsize>(expected.size()));
    std::printf("regenerated %s (%zu bytes)\n", path, expected.size());
    return 0;
  }

  std::string committed = ReadFile(path);
  if (committed != expected) {
    std::printf("FAIL  order wire drift; regenerate with KAIROS_REGEN=1\n");
    return 1;
  }

  std::vector<std::vector<std::uint8_t>> frames = Frames(committed);
  CHECK_EQ(static_cast<int>(frames.size()), 5);
  OrderMessage out;

  CHECK(DecodeOrder(frames[0].data(), frames[0].size(), &out));
  CHECK(out.kind == OrderMsgKind::kSubmit);
  CHECK(out.submit.id == "k12345-7");
  CHECK(out.submit.symbol == "2330");
  CHECK(out.submit.market == Market::kOtc);
  CHECK(out.submit.board == Board::kRoundLot);
  CHECK(out.submit.side == Side::kSell);
  CHECK(out.submit.funding_type == "Cash");
  CHECK(out.submit.time_in_force == "ROD");
  CHECK_EQ(out.submit.price, 92500);
  CHECK_EQ(out.submit.shares, 2000);

  CHECK(DecodeOrder(frames[1].data(), frames[1].size(), &out));
  CHECK(out.kind == OrderMsgKind::kCancel);
  CHECK(out.cancel.id == "k12345-7");

  CHECK(DecodeOrder(frames[2].data(), frames[2].size(), &out));
  CHECK(out.kind == OrderMsgKind::kAck);
  CHECK(out.ack.id == "k12345-7");
  CHECK(!out.ack.ok);
  CHECK(out.ack.error_message == "rejected by broker");

  CHECK(DecodeOrder(frames[3].data(), frames[3].size(), &out));
  CHECK(out.kind == OrderMsgKind::kFill);
  CHECK(out.fill.id == "k12345-7");
  CHECK_EQ(out.fill.shares, 1000);
  CHECK_EQ(out.fill.price, 92500);

  CHECK(DecodeOrder(frames[4].data(), frames[4].size(), &out));
  CHECK(out.kind == OrderMsgKind::kCancelResult);
  CHECK(out.cancel_result.id == "k12345-7");
  CHECK(out.cancel_result.ok);

  if (g_failures != 0) std::printf("%d failures\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
