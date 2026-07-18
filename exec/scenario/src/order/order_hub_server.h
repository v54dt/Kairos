#ifndef KAIROS_EXEC_ORDER_HUB_SERVER_H_
#define KAIROS_EXEC_ORDER_HUB_SERVER_H_

#include <atomic>
#include <cstdint>
#include <memory>
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

  // Shut down every live client connection while staying bound and accepting, so a
  // reconnect is served. Sim-only fault hook (kairos_sim_hubd's disconnect drills);
  // the live hub never calls it.
  void DisconnectAllClients();

 private:
  void AcceptLoop();
  void ClientLoop(int fd);
  void Send(int client, const std::vector<std::uint8_t>& bytes);
  // Best-effort, never-blocking `forwarded` hint: dropped if the client's write
  // lock is contended or its socket buffer is full, so the shared forwarder thread
  // that calls it is never stalled by a slow reader.
  void TrySendForwarded(int client, const std::vector<std::uint8_t>& bytes);
  void StatusLoop();                          // periodic status snapshot writer
  void WriteStatus(const std::string& path);  // one snapshot, off all mutexes

  std::string path_;
  OrderHub hub_;
  int listen_fd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread accept_thread_;
  std::thread status_thread_;
  std::mutex clients_mu_;
  std::unordered_set<int> live_;  // open client fds (Send guard)
  // Per-client write lock: serializes every reply to one fd so a forwarder-thread
  // write and an SDK-callback-thread write never interleave and tear each other's
  // length-prefixed frames. shared_ptr so an in-flight Send keeps it past a close.
  std::unordered_map<int, std::shared_ptr<std::mutex>> write_mu_;
  std::atomic<int> active_clients_{0};  // detached client threads in flight
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ORDER_HUB_SERVER_H_
