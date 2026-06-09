#include "order_hub_server.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "uds_frame.h"

namespace kairos::exec {

OrderHubServer::OrderHubServer(OrderBackend* backend, std::string path)
    : path_(std::move(path)),
      hub_(backend, [this](int c, const std::vector<std::uint8_t>& b) { Send(c, b); }) {}

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
  std::printf("kairos-order-hub: listening on %s\n", path_.c_str());
  std::fflush(stdout);
  return true;
}

void OrderHubServer::AcceptLoop() {
  while (!stop_) {
    int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (stop_) break;
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(clients_mu_);
      live_.insert(fd);
    }
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
  }
  ::close(fd);
}

void OrderHubServer::Send(int client, const std::vector<std::uint8_t>& bytes) {
  std::lock_guard<std::mutex> lock(clients_mu_);
  if (live_.count(client)) WriteFrame(client, bytes);
}

void OrderHubServer::Stop() {
  if (stop_.exchange(true)) return;  // idempotent
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (accept_thread_.joinable()) accept_thread_.join();

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
