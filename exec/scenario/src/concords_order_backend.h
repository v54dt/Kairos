#ifndef KAIROS_EXEC_CONCORDS_ORDER_BACKEND_H_
#define KAIROS_EXEC_CONCORDS_ORDER_BACKEND_H_

#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include "order_backend.h"
#include "scenario.h"
#include "stock/include/stock.h"

namespace kairos::exec {

// Real order backend over the concords StockClient. Every SDK call is serialized
// through a 1 req/s gate (concords rate limit). Order ids are the engine's
// correlation ids (concords user_defined_id); cancel/update target the same id.
class ConcordsOrderBackend : public OrderBackend {
 public:
  explicit ConcordsOrderBackend(Scenario s);

  bool Connect() override;
  void Disconnect() override;
  bool IsConnected() const override;
  void Submit(const std::string& id, Side side, Cents price, long shares) override;
  void Cancel(const std::string& id) override;

 private:
  void Gate();

  Scenario s_;
  std::unique_ptr<concords_sdk::stock::StockClient> stock_;
  std::mutex gate_mu_;
  std::chrono::steady_clock::time_point last_req_{};
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_CONCORDS_ORDER_BACKEND_H_
