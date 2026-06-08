#ifndef KAIROS_EXEC_HUB_ORDER_BACKEND_H_
#define KAIROS_EXEC_HUB_ORDER_BACKEND_H_

#include <atomic>
#include <string>
#include <thread>

#include "order_backend.h"

namespace kairos::exec {

// OrderBackend that forwards to the shared order hub over UDS instead of driving
// the SDK directly: Submit/Cancel are sent as frames; the hub's ack/fill/cancel
// arrive on a reader thread and fire the callbacks. The hub owns the concords
// connection and the 1 req/s gate, so this client does not gate.
class HubOrderBackend : public OrderBackend {
 public:
  explicit HubOrderBackend(std::string socket_path);
  ~HubOrderBackend() override;

  bool Connect() override;
  void Disconnect() override;
  bool IsConnected() const override;
  void Submit(const OrderSubmitMsg& order) override;
  void Cancel(const std::string& id) override;

 private:
  void ReadLoop();

  std::string path_;
  std::atomic<int> fd_{-1};
  std::atomic<bool> stop_{false};
  std::thread reader_;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_HUB_ORDER_BACKEND_H_
