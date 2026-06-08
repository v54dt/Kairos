// Self-test for the order wire codec: encode -> decode roundtrip for each
// OrderEnvelope variant. No socket, no broker.

#include <capnp/serialize.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "order_codec.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

int main() {
  // submit roundtrip carries the full order spec
  {
    OrderSubmitMsg m;
    m.id = "k12345-7";
    m.symbol = "2330";
    m.market = Market::kOtc;
    m.board = Board::kRoundLot;
    m.side = Side::kSell;
    m.funding_type = "Cash";
    m.time_in_force = "ROD";
    m.price = 92500;  // 925.00
    m.shares = 2000;

    auto bytes = EncodeOrderSubmit(m);
    CHECK(bytes.size() % sizeof(capnp::word) == 0);
    OrderMessage out;
    CHECK(DecodeOrder(bytes.data(), bytes.size(), &out));
    CHECK(out.kind == OrderMsgKind::kSubmit);
    CHECK(out.submit.id == "k12345-7");
    CHECK(out.submit.symbol == "2330");
    CHECK(out.submit.market == Market::kOtc);
    CHECK(out.submit.board == Board::kRoundLot);
    CHECK(out.submit.side == Side::kSell);
    CHECK(out.submit.funding_type == "Cash");
    CHECK(out.submit.time_in_force == "ROD");
    CHECK(out.submit.price == 92500);
    CHECK(out.submit.shares == 2000);
  }

  // cancel roundtrip
  {
    auto bytes = EncodeOrderCancel({"k12345-7"});
    OrderMessage out;
    CHECK(DecodeOrder(bytes.data(), bytes.size(), &out));
    CHECK(out.kind == OrderMsgKind::kCancel);
    CHECK(out.cancel.id == "k12345-7");
  }

  // ack roundtrip (with error message)
  {
    auto bytes = EncodeOrderAck({"k12345-7", false, "rejected by broker"});
    OrderMessage out;
    CHECK(DecodeOrder(bytes.data(), bytes.size(), &out));
    CHECK(out.kind == OrderMsgKind::kAck);
    CHECK(out.ack.id == "k12345-7");
    CHECK(!out.ack.ok);
    CHECK(out.ack.error_message == "rejected by broker");
  }

  // fill roundtrip
  {
    auto bytes = EncodeOrderFill({"k12345-7", 1000, 92500});
    OrderMessage out;
    CHECK(DecodeOrder(bytes.data(), bytes.size(), &out));
    CHECK(out.kind == OrderMsgKind::kFill);
    CHECK(out.fill.id == "k12345-7");
    CHECK(out.fill.shares == 1000);
    CHECK(out.fill.price == 92500);
  }

  // cancel-result roundtrip
  {
    auto bytes = EncodeOrderCancelResult({"k12345-7", true});
    OrderMessage out;
    CHECK(DecodeOrder(bytes.data(), bytes.size(), &out));
    CHECK(out.kind == OrderMsgKind::kCancelResult);
    CHECK(out.cancel_result.id == "k12345-7");
    CHECK(out.cancel_result.ok);
  }

  // malformed input -> false
  {
    std::vector<std::uint8_t> junk = {1, 2, 3};
    OrderMessage out;
    CHECK(!DecodeOrder(junk.data(), junk.size(), &out));
    CHECK(!DecodeOrder(nullptr, 0, &out));
  }

  if (g_failures == 0) {
    std::printf("test_order_codec: OK\n");
    return 0;
  }
  std::printf("test_order_codec: FAILED %d check(s)\n", g_failures);
  return 1;
}
