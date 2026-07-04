#include "uds_quote_client.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#include "quote_codec.h"
#include "uds_frame.h"

namespace kairos::exec {

namespace {
constexpr auto kBackoffMin = std::chrono::milliseconds(200);
constexpr auto kBackoffMax = std::chrono::seconds(5);

void InterruptibleSleep(std::chrono::milliseconds d, const std::atomic<bool>& stop) {
  auto until = std::chrono::steady_clock::now() + d;
  while (!stop && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}
}  // namespace

UdsQuoteClient::UdsQuoteClient(std::string socket_path, std::vector<std::string> symbols,
                               QuoteFn on_quote)
    : socket_path_(std::move(socket_path)),
      symbols_(std::move(symbols)),
      on_quote_(std::move(on_quote)) {}

UdsQuoteClient::~UdsQuoteClient() { Stop(); }

void UdsQuoteClient::Start() {
  thread_ = std::thread([this] { Run(); });
}

void UdsQuoteClient::Stop() {
  stop_ = true;
  int fd = fd_.exchange(-1);
  if (fd >= 0) ::shutdown(fd, SHUT_RDWR);  // unblock a pending read
  if (thread_.joinable()) thread_.join();
}

int UdsQuoteClient::ConnectAndSubscribe() {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  // Non-blocking connect so Stop() can abort a server that is slow to accept.
  int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (errno != EINPROGRESS) {
      ::close(fd);
      return -1;
    }
    pollfd pfd{fd, POLLOUT, 0};
    bool connected = false;
    while (!stop_ && !connected) {
      int p = ::poll(&pfd, 1, 100);
      if (p < 0) {
        if (errno == EINTR) continue;
        ::close(fd);
        return -1;
      }
      if (p == 0) continue;  // 100ms tick: re-check stop_
      int err = 0;
      socklen_t len = sizeof(err);
      ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
      if (err != 0) {
        ::close(fd);
        return -1;
      }
      connected = true;
    }
    if (!connected) {  // stop_ requested mid-connect
      ::close(fd);
      return -1;
    }
  }
  ::fcntl(fd, F_SETFL, flags);  // restore blocking for WriteFrame + the read loop

  fd_ = fd;  // publish before the blocking write so Stop() can shutdown() it
  if (!WriteFrame(fd, EncodeSubscribe(symbols_))) {
    fd_.store(-1);
    ::close(fd);
    return -1;
  }
  return fd;
}

void UdsQuoteClient::Run() {
  auto backoff = kBackoffMin;
  while (!stop_) {
    int fd = ConnectAndSubscribe();
    if (fd < 0) {
      InterruptibleSleep(backoff, stop_);
      backoff =
          std::min(backoff * 2, std::chrono::duration_cast<std::chrono::milliseconds>(kBackoffMax));
      continue;
    }
    // fd_ was published inside ConnectAndSubscribe (before the subscribe write)
    backoff = kBackoffMin;

    std::vector<std::uint8_t> frame;
    while (!stop_) {
      int r = ReadFrame(fd, &frame);
      if (r <= 0) break;  // EOF or error -> reconnect
      // With no trade callback the live path never calls DecodeTrade, so the
      // quote code path and the unknown-variant counter are exactly as before.
      std::string symbol;
      Trade trade;
      if (on_trade_ && DecodeTrade(frame.data(), frame.size(), &trade, &symbol)) {
        on_trade_(symbol, trade);
        continue;
      }
      TopOfBook tob;
      if (DecodeQuote(frame.data(), frame.size(), &tob, &symbol)) on_quote_(symbol, tob);
    }
    int old = fd_.exchange(-1);
    if (old >= 0) ::close(old);
  }
}

}  // namespace kairos::exec
