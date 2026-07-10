#ifndef KAIROS_EXEC_CONCORDS_ORDER_BACKEND_H_
#define KAIROS_EXEC_CONCORDS_ORDER_BACKEND_H_

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

#include "order_backend.h"
#include "scenario.h"
#include "stock/include/stock.h"

namespace kairos::exec {

// Real order backend over the concords StockClient. Every SDK call is serialized
// through a 1 req/s gate (concords rate limit). Order ids are the engine's
// correlation ids (concords user_defined_id); submit/ack/fill all key on it.
// CancelOrder's parameter is the SDK's broker target_id and OrderSubmitResult
// never returns one, so we pass the user_defined_id and flag any cancel ack whose
// id does not echo it (whether the broker cancels by user_defined_id is unverified).
// Holds only creds: the full order spec arrives per Submit, so one backend can
// serve any symbol (used directly by a scenario, or shared by the order hub).
class ConcordsOrderBackend : public OrderBackend {
 public:
  explicit ConcordsOrderBackend(UserCreds creds);

  bool Connect() override;
  void Disconnect() override;
  bool IsConnected() const override;
  void Submit(const OrderSubmitMsg& order) override;
  void Cancel(const std::string& id) override;

 private:
  void Gate();

  UserCreds creds_;
  std::unique_ptr<concords_sdk::stock::StockClient> stock_;
  std::mutex gate_mu_;
  std::chrono::steady_clock::time_point last_req_{};
  std::mutex cancel_mu_;
  std::unordered_set<std::string> pending_cancels_;  // user_defined_ids we asked to cancel
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_CONCORDS_ORDER_BACKEND_H_
