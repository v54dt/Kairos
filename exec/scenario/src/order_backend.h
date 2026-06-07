#ifndef KAIROS_EXEC_ORDER_BACKEND_H_
#define KAIROS_EXEC_ORDER_BACKEND_H_

#include <functional>
#include <string>

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

  virtual ~OrderBackend() = default;

  virtual bool Connect() = 0;
  virtual void Disconnect() = 0;
  virtual bool IsConnected() const = 0;
  virtual std::string Submit(Side side, Cents price, long shares) = 0;
  virtual void UpdatePrice(const std::string& id, Cents new_price) = 0;  // re-peg
  virtual void Cancel(const std::string& id) = 0;

  void SetCallbacks(AckFn ack, FillFn fill, CancelFn cancel) {
    on_ack_ = std::move(ack);
    on_fill_ = std::move(fill);
    on_cancel_ = std::move(cancel);
  }

 protected:
  AckFn on_ack_;
  FillFn on_fill_;
  CancelFn on_cancel_;
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

  std::string Submit(Side /*side*/, Cents price, long shares) override {
    std::string id = "paper-" + std::to_string(++seq_);
    if (on_ack_) on_ack_(id, true, "");
    if (on_fill_) on_fill_(id, Fill{shares, price});
    return id;
  }

  void UpdatePrice(const std::string& /*id*/, Cents /*new_price*/) override {}

  void Cancel(const std::string& id) override {
    if (on_cancel_) on_cancel_(id, true);
  }

 private:
  bool connected_ = false;
  long seq_ = 0;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ORDER_BACKEND_H_
