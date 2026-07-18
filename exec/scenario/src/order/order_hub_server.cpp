#include "order_hub_server.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "hub_status.h"
#include "uds_frame.h"

namespace kairos::exec {

OrderHubServer::OrderHubServer(OrderBackend* backend, std::string path, OrderHub::RiskConfig risk)
    : path_(std::move(path)),
      hub_(
          backend, [this](int c, const std::vector<std::uint8_t>& b) { Send(c, b); },
          std::move(risk)) {}

OrderHubServer::~OrderHubServer() { Stop(); }

bool OrderHubServer::Start() {
  if (!hub_.Start()) {
    std::fprintf(stderr, "kairos-order-hub: backend connect failed\n");
    return false;
  }
  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return false;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
  ::unlink(path_.c_str());  // clear a stale socket
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(listen_fd_, 16) != 0) {
    std::fprintf(stderr, "kairos-order-hub: bind/listen %s failed\n", path_.c_str());
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  accept_thread_ = std::thread([this] { AcceptLoop(); });
  status_thread_ = std::thread([this] { StatusLoop(); });
  std::printf("kairos-order-hub: listening on %s\n", path_.c_str());
  std::fflush(stdout);
  return true;
}

void OrderHubServer::WriteStatus(const std::string& path) {
  AtomicWriteFile(path, SerializeHubStatus(hub_.CaptureStatus()));
}

void OrderHubServer::StatusLoop() {
  const std::string path = HubStatusPath();
  if (path.empty()) return;  // no runtime dir: skip status writes, never /tmp
  while (!stop_) {
    WriteStatus(path);
    for (int i = 0; i < 40 && !stop_; ++i)  // ~2s, sliced for a responsive shutdown
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

void OrderHubServer::AcceptLoop() {
  while (!stop_) {
    int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (stop_) break;
      continue;
    }
    timeval tv{5, 0};  // bound writes: a stuck reader can't wedge the hub forever
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    {
      std::lock_guard<std::mutex> lock(clients_mu_);
      live_.insert(fd);
      write_mu_[fd] = std::make_shared<std::mutex>();
    }
    hub_.OnClientConnect(fd);
    ++active_clients_;
    std::thread([this, fd] {
      ClientLoop(fd);
      --active_clients_;
    }).detach();
  }
}

void OrderHubServer::ClientLoop(int fd) {
  std::vector<std::uint8_t> frame;
  while (!stop_) {
    int r = ReadFrame(fd, &frame);
    if (r <= 0) break;  // EOF or error
    hub_.OnClientMessage(fd, frame.data(), frame.size());
  }
  hub_.OnClientDisconnect(fd);
  {
    std::lock_guard<std::mutex> lock(clients_mu_);
    live_.erase(fd);
    write_mu_.erase(fd);
  }
  ::close(fd);
}

void OrderHubServer::Send(int client, const std::vector<std::uint8_t>& bytes) {
  int dupfd = -1;
  std::shared_ptr<std::mutex> wmu;
  {
    std::lock_guard<std::mutex> lock(clients_mu_);
    if (live_.count(client)) {
      dupfd = ::dup(client);  // keep the socket alive past a concurrent close
      wmu = write_mu_[client];
    }
  }
  if (dupfd < 0) {
    std::fprintf(stderr, "kairos-order-hub: drop reply to client=%d (connection gone)\n", client);
    return;
  }
  // Off clients_mu_ so a slow client can't stall accept/disconnect/Stop, but under
  // the per-client write lock so two concurrent replies to one fd (forwarder vs
  // SDK-callback thread) never interleave and tear each other's framed payloads.
  bool ok;
  {
    std::lock_guard<std::mutex> wlock(*wmu);
    ok = WriteFrame(dupfd, bytes);
  }
  if (!ok) std::fprintf(stderr, "kairos-order-hub: reply write to client=%d failed\n", client);
  ::close(dupfd);
}

void OrderHubServer::DisconnectAllClients() {
  std::vector<int> fds;
  {
    std::lock_guard<std::mutex> lock(clients_mu_);
    fds.assign(live_.begin(), live_.end());
  }
  // Unblock each client reader; its ClientLoop sees EOF, runs OnClientDisconnect,
  // and closes the fd. The listen socket stays open, so a reconnect is accepted.
  for (int fd : fds) ::shutdown(fd, SHUT_RDWR);
}

void OrderHubServer::Stop() {
  if (stop_.exchange(true)) return;  // idempotent
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (accept_thread_.joinable()) accept_thread_.join();
  if (status_thread_.joinable()) status_thread_.join();

  // Snapshot while the registry is still intact: tearing down the client fds
  // makes each ClientLoop call OnClientDisconnect, which erases per-client
  // state, so capturing after the drain would clobber it with an empty file.
  const std::string status = HubStatusPath();
  if (!status.empty()) WriteStatus(status);  // final snapshot of what was connected/open

  std::vector<int> fds;
  {
    std::lock_guard<std::mutex> lock(clients_mu_);
    fds.assign(live_.begin(), live_.end());
  }
  for (int fd : fds) ::shutdown(fd, SHUT_RDWR);  // unblock pending reads
  while (active_clients_.load() > 0) {           // wait for detached client threads to drain
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  hub_.Stop();
  ::unlink(path_.c_str());
}

}  // namespace kairos::exec
