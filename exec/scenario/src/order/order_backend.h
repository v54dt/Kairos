#ifndef KAIROS_EXEC_ORDER_BACKEND_H_
#define KAIROS_EXEC_ORDER_BACKEND_H_

#include <functional>
#include <string>

#include "order_codec.h"  // OrderSubmitMsg (the full order spec)
#include "scenario.h"
#include "tw_market.h"

namespace kairos::exec {

struct Fill {
  long shares = 0;
  Cents price = 0;
};

// Broker-agnostic order interface the engine drives. Concrete backends: a paper
// simulator (below) and the SDK-bound concords backend (added in C1b).
class OrderBackend {
 public:
  using AckFn = std::function<void(const std::string& id, bool ok, const std::string& err)>;
  using FillFn = std::function<void(const std::string& id, const Fill&)>;
  using CancelFn = std::function<void(const std::string& id, bool ok)>;
  using DisconnectFn = std::function<void()>;  // backend lost its link unexpectedly

  virtual ~OrderBackend() = default;

  virtual bool Connect() = 0;
  virtual void Disconnect() = 0;
  virtual bool IsConnected() const = 0;
  // Full order spec: order.id is the correlation id (concords user_defined_id) so
  // callbacks match even on synchronous fills, and one backend can serve any symbol.
  virtual void Submit(const OrderSubmitMsg& order) = 0;
  virtual void Cancel(const std::string& id) = 0;  // also used to re-peg (cancel + re-place)

  void SetCallbacks(AckFn ack, FillFn fill, CancelFn cancel, DisconnectFn disconnect = {}) {
    on_ack_ = std::move(ack);
    on_fill_ = std::move(fill);
    on_cancel_ = std::move(cancel);
    on_disconnect_ = std::move(disconnect);
  }

 protected:
  AckFn on_ack_;
  FillFn on_fill_;
  CancelFn on_cancel_;
  DisconnectFn on_disconnect_;
};

// Paper backend: acks and fully fills every order at its limit price, instantly.
// Used by PAPER mode (no order socket) and engine unit tests.
class PaperOrderBackend : public OrderBackend {
 public:
  bool Connect() override {
    connected_ = true;
    return true;
  }
  void Disconnect() override { connected_ = false; }
  bool IsConnected() const override { return connected_; }

  void Submit(const OrderSubmitMsg& o) override {
    if (on_ack_) on_ack_(o.id, true, "");
    if (on_fill_) on_fill_(o.id, Fill{o.shares, o.price});
  }

  void Cancel(const std::string& id) override {
    if (on_cancel_) on_cancel_(id, true);
  }

 private:
  bool connected_ = false;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ORDER_BACKEND_H_
