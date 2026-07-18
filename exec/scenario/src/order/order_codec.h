#ifndef KAIROS_EXEC_ORDER_CODEC_H_
#define KAIROS_EXEC_ORDER_CODEC_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "scenario.h"   // Side, Market, Board
#include "tw_market.h"  // Cents

namespace kairos::exec {

// Full order spec the scenario sends to the hub: one hub serves many symbols, so
// the request carries symbol/market/board/funding/TIF rather than relying on a
// single stored Scenario.
struct OrderSubmitMsg {
  std::string id;  // user_defined_id (k<pid>-<seq>)
  std::string symbol;
  Market market = Market::kTse;
  Board board = Board::kOddLot;
  Side side = Side::kBuy;
  std::string funding_type;
  std::string time_in_force;
  Cents price = 0;
  long shares = 0;  // raw shares; hub converts by board
};

struct OrderCancelMsg {
  std::string id;
};

// Hub -> scenario events; map onto the OrderBackend callbacks.
struct OrderAckMsg {
  std::string id;
  bool ok = false;
  std::string error_message;
};

struct OrderFillMsg {
  std::string id;
  long shares = 0;
  Cents price = 0;
};

struct OrderCancelResultMsg {
  std::string id;
  bool ok = false;
};

// Hub -> owning scenario: the queued submit cleared the forwarder gate and is
// being handed to the broker now, so the trader can rebase its ack-timeout clock.
struct OrderForwardedMsg {
  std::string id;
};

enum class OrderMsgKind {
  kNone,
  kSubmit,
  kCancel,
  kAck,
  kFill,
  kCancelResult,
  kHeartbeat,
  kForwarded
};

// One decoded OrderEnvelope; `kind` selects which member is populated.
struct OrderMessage {
  OrderMsgKind kind = OrderMsgKind::kNone;
  OrderSubmitMsg submit;
  OrderCancelMsg cancel;
  OrderAckMsg ack;
  OrderFillMsg fill;
  OrderCancelResultMsg cancel_result;
  OrderForwardedMsg forwarded;
};

std::vector<std::uint8_t> EncodeOrderSubmit(const OrderSubmitMsg& m);
std::vector<std::uint8_t> EncodeOrderCancel(const OrderCancelMsg& m);
std::vector<std::uint8_t> EncodeOrderAck(const OrderAckMsg& m);
std::vector<std::uint8_t> EncodeOrderFill(const OrderFillMsg& m);
std::vector<std::uint8_t> EncodeOrderCancelResult(const OrderCancelResultMsg& m);
std::vector<std::uint8_t> EncodeOrderForwarded(const OrderForwardedMsg& m);

// Decode a serialized OrderEnvelope into *out. Returns false on malformed input.
bool DecodeOrder(const std::uint8_t* data, std::size_t len, OrderMessage* out);

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ORDER_CODEC_H_
