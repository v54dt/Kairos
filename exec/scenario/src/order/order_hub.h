#ifndef KAIROS_EXEC_ORDER_HUB_H_
#define KAIROS_EXEC_ORDER_HUB_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "order_backend.h"

namespace kairos::exec {

// Routing core of the concords order hub: many scenario clients submit/cancel
// through one shared OrderBackend (one concords connection, one 1 req/s gate).
// Tracks which client owns each user_defined_id and routes the backend's
// ack/fill/cancel events back to that client. Socket-agnostic: the server
// injects a SendFn, so the routing is unit-testable without sockets.
class OrderHub {
 public:
  // Deliver a serialized OrderEnvelope to client `client`.
  using SendFn = std::function<void(int client, const std::vector<std::uint8_t>&)>;

  OrderHub(OrderBackend* backend, SendFn send);

  bool Start();  // wire backend callbacks + connect
  void Stop();   // disconnect the backend

  // Feed one client frame (a serialized OrderEnvelope). Malformed frames and
  // hub->client message kinds (ack/fill/...) are ignored.
  void OnClientMessage(int client, const std::uint8_t* data, std::size_t len);
  // Drop all routing for a client whose connection went away.
  void OnClientDisconnect(int client);

 private:
  void OnAck(const std::string& id, bool ok, const std::string& err);
  void OnFill(const std::string& id, const Fill& f);
  void OnCancel(const std::string& id, bool ok);
  int ClientFor(const std::string& id);  // -1 if unknown

  OrderBackend* backend_;
  SendFn send_;
  std::mutex mu_;
  std::unordered_map<std::string, int> id_to_client_;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ORDER_HUB_H_
