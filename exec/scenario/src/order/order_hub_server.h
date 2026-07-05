#ifndef KAIROS_EXEC_ORDER_HUB_SERVER_H_
#define KAIROS_EXEC_ORDER_HUB_SERVER_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "order_backend.h"
#include "order_hub.h"

namespace kairos::exec {

// UDS front end for the order hub: listens on `path`, gives each connection a
// reader thread that feeds frames to the OrderHub, and writes the hub's routed
// events back to the owning connection. The client fd is the OrderHub's client id.
class OrderHubServer {
 public:
  OrderHubServer(OrderBackend* backend, std::string path, OrderHub::RiskConfig risk = {});
  ~OrderHubServer();

  bool Start();  // connect the backend, then bind/listen/accept
  void Stop();

 private:
  void AcceptLoop();
  void ClientLoop(int fd);
  void Send(int client, const std::vector<std::uint8_t>& bytes);
  void StatusLoop();                          // periodic status snapshot writer
  void WriteStatus(const std::string& path);  // one snapshot, off all mutexes

  std::string path_;
  OrderHub hub_;
  int listen_fd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread accept_thread_;
  std::thread status_thread_;
  std::mutex clients_mu_;
  std::unordered_set<int> live_;        // open client fds (Send guard)
  std::atomic<int> active_clients_{0};  // detached client threads in flight
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ORDER_HUB_SERVER_H_
