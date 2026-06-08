#include "hub_order_backend.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <utility>
#include <vector>

#include "order_codec.h"
#include "uds_frame.h"

namespace kairos::exec {

HubOrderBackend::HubOrderBackend(std::string socket_path) : path_(std::move(socket_path)) {}

HubOrderBackend::~HubOrderBackend() { Disconnect(); }

bool HubOrderBackend::Connect() {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return false;
  }
  fd_ = fd;
  stop_ = false;
  reader_ = std::thread([this] { ReadLoop(); });
  return true;
}

void HubOrderBackend::Disconnect() {
  stop_ = true;
  int fd = fd_.exchange(-1);
  if (fd >= 0) ::shutdown(fd, SHUT_RDWR);  // unblock a pending read
  if (reader_.joinable()) reader_.join();
  if (fd >= 0) ::close(fd);
}

bool HubOrderBackend::IsConnected() const { return fd_ >= 0; }

void HubOrderBackend::Submit(const OrderSubmitMsg& order) {
  int fd = fd_;
  if (fd >= 0) WriteFrame(fd, EncodeOrderSubmit(order));
}

void HubOrderBackend::Cancel(const std::string& id) {
  int fd = fd_;
  if (fd >= 0) WriteFrame(fd, EncodeOrderCancel({id}));
}

void HubOrderBackend::ReadLoop() {
  std::vector<std::uint8_t> frame;
  while (!stop_) {
    int fd = fd_;
    if (fd < 0) break;
    int r = ReadFrame(fd, &frame);
    if (r <= 0) break;  // EOF or error -> hub gone
    OrderMessage m;
    if (!DecodeOrder(frame.data(), frame.size(), &m)) continue;
    switch (m.kind) {
      case OrderMsgKind::kAck:
        if (on_ack_) on_ack_(m.ack.id, m.ack.ok, m.ack.error_message);
        break;
      case OrderMsgKind::kFill:
        if (on_fill_) on_fill_(m.fill.id, Fill{m.fill.shares, m.fill.price});
        break;
      case OrderMsgKind::kCancelResult:
        if (on_cancel_) on_cancel_(m.cancel_result.id, m.cancel_result.ok);
        break;
      default:
        break;
    }
  }
}

}  // namespace kairos::exec
