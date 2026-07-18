#include "order_codec.h"

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <cstring>

#include "kairos.capnp.h"

namespace kairos::exec {

namespace {

// capnp-generated types (Market/Side/Board/OrderEnvelope) live in the global
// namespace and clash with kairos::exec::{Market,Side,Board}, so qualify them.
std::vector<std::uint8_t> Flatten(capnp::MessageBuilder& msg) {
  auto flat = capnp::messageToFlatArray(msg);
  auto bytes = flat.asBytes();
  return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

}  // namespace

std::vector<std::uint8_t> EncodeOrderSubmit(const OrderSubmitMsg& m) {
  capnp::MallocMessageBuilder msg;
  auto s = msg.initRoot<OrderEnvelope>().initSubmit();
  s.setId(m.id.c_str());
  s.setSymbol(m.symbol.c_str());
  s.setMarket(m.market == Market::kTse ? ::Market::TSE : ::Market::OTC);
  s.setBoard(m.board == Board::kOddLot ? ::Board::ODD_LOT : ::Board::ROUND_LOT);
  s.setSide(m.side == Side::kBuy ? ::Side::BUY : ::Side::SELL);
  s.setFundingType(m.funding_type.c_str());
  s.setTimeInForce(m.time_in_force.c_str());
  s.setPriceCents(m.price);
  s.setShares(m.shares);
  return Flatten(msg);
}

std::vector<std::uint8_t> EncodeOrderCancel(const OrderCancelMsg& m) {
  capnp::MallocMessageBuilder msg;
  msg.initRoot<OrderEnvelope>().initCancel().setId(m.id.c_str());
  return Flatten(msg);
}

std::vector<std::uint8_t> EncodeOrderAck(const OrderAckMsg& m) {
  capnp::MallocMessageBuilder msg;
  auto a = msg.initRoot<OrderEnvelope>().initAck();
  a.setId(m.id.c_str());
  a.setOk(m.ok);
  a.setErrorMessage(m.error_message.c_str());
  return Flatten(msg);
}

std::vector<std::uint8_t> EncodeOrderFill(const OrderFillMsg& m) {
  capnp::MallocMessageBuilder msg;
  auto f = msg.initRoot<OrderEnvelope>().initFill();
  f.setId(m.id.c_str());
  f.setShares(m.shares);
  f.setPriceCents(m.price);
  return Flatten(msg);
}

std::vector<std::uint8_t> EncodeOrderCancelResult(const OrderCancelResultMsg& m) {
  capnp::MallocMessageBuilder msg;
  auto r = msg.initRoot<OrderEnvelope>().initCancelResult();
  r.setId(m.id.c_str());
  r.setOk(m.ok);
  return Flatten(msg);
}

std::vector<std::uint8_t> EncodeOrderForwarded(const OrderForwardedMsg& m) {
  capnp::MallocMessageBuilder msg;
  msg.initRoot<OrderEnvelope>().initForwarded().setId(m.id.c_str());
  return Flatten(msg);
}

bool DecodeOrder(const std::uint8_t* data, std::size_t len, OrderMessage* out) {
  if (data == nullptr || len == 0 || len % sizeof(capnp::word) != 0) {
    return false;
  }
  try {
    std::vector<capnp::word> words(len / sizeof(capnp::word));
    std::memcpy(words.data(), data, len);
    capnp::FlatArrayMessageReader reader(kj::arrayPtr(words.data(), words.size()));
    auto env = reader.getRoot<OrderEnvelope>();
    switch (env.which()) {
      case OrderEnvelope::SUBMIT: {
        auto s = env.getSubmit();
        out->kind = OrderMsgKind::kSubmit;
        out->submit.id = s.getId().cStr();
        out->submit.symbol = s.getSymbol().cStr();
        out->submit.market = s.getMarket() == ::Market::TSE ? Market::kTse : Market::kOtc;
        out->submit.board = s.getBoard() == ::Board::ODD_LOT ? Board::kOddLot : Board::kRoundLot;
        out->submit.side = s.getSide() == ::Side::BUY ? Side::kBuy : Side::kSell;
        out->submit.funding_type = s.getFundingType().cStr();
        out->submit.time_in_force = s.getTimeInForce().cStr();
        out->submit.price = s.getPriceCents();
        out->submit.shares = s.getShares();
        return true;
      }
      case OrderEnvelope::CANCEL:
        out->kind = OrderMsgKind::kCancel;
        out->cancel.id = env.getCancel().getId().cStr();
        return true;
      case OrderEnvelope::ACK: {
        auto a = env.getAck();
        out->kind = OrderMsgKind::kAck;
        out->ack.id = a.getId().cStr();
        out->ack.ok = a.getOk();
        out->ack.error_message = a.getErrorMessage().cStr();
        return true;
      }
      case OrderEnvelope::FILL: {
        auto f = env.getFill();
        out->kind = OrderMsgKind::kFill;
        out->fill.id = f.getId().cStr();
        out->fill.shares = f.getShares();
        out->fill.price = f.getPriceCents();
        return true;
      }
      case OrderEnvelope::CANCEL_RESULT: {
        auto r = env.getCancelResult();
        out->kind = OrderMsgKind::kCancelResult;
        out->cancel_result.id = r.getId().cStr();
        out->cancel_result.ok = r.getOk();
        return true;
      }
      case OrderEnvelope::HEARTBEAT:
        out->kind = OrderMsgKind::kHeartbeat;
        return true;
      case OrderEnvelope::FORWARDED:
        out->kind = OrderMsgKind::kForwarded;
        out->forwarded.id = env.getForwarded().getId().cStr();
        return true;
    }
    return false;
  } catch (...) {
    return false;
  }
}

}  // namespace kairos::exec
