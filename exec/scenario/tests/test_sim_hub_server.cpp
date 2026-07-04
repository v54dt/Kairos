// Integration: kairos_sim_hubd's SimOrderBackend behind the REAL OrderHubServer
// over a UDS socket. A client submits through the REAL EncodeOrderSubmit codec
// (the same one HubOrderBackend uses) and must get the routed ack + deterministic
// fills for a continuous case and a closing-auction case. No broker.

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "order_codec.h"
#include "order_hub_server.h"
#include "quote_book.h"
#include "sim_order_backend.h"
#include "uds_frame.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

namespace {

std::int64_t Us(int hh, int mm, int ss = 0) {
  long local = hh * 3600 + mm * 60 + ss;
  return static_cast<std::int64_t>(local - 8 * 3600) * 1000000;
}

int ConnectClient(const std::string& path) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  timeval tv{2, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

bool ReadMsg(int fd, OrderMessage* out) {
  std::vector<std::uint8_t> frame;
  if (ReadFrame(fd, &frame) != 1) return false;
  return DecodeOrder(frame.data(), frame.size(), out);
}

TopOfBook Book(std::vector<Level> bids, std::vector<Level> asks, std::int64_t ts) {
  TopOfBook t;
  for (auto& b : bids) {
    if (t.n_bids >= TopOfBook::kMaxLevels) break;
    t.bids[t.n_bids++] = b;
  }
  for (auto& a : asks) {
    if (t.n_asks >= TopOfBook::kMaxLevels) break;
    t.asks[t.n_asks++] = a;
  }
  t.quote_ts_us = ts;
  t.valid = true;
  return t;
}

OrderSubmitMsg Order(const std::string& id, Side side, Cents price, long shares) {
  return {id, "2330", Market::kTse, Board::kRoundLot, side, "Cash", "ROD", price, shares};
}

void TestContinuous() {
  std::string path = "/tmp/kairos-test-simhub-c-" + std::to_string(::getpid()) + ".sock";
  SimOrderBackend backend(FillMode::kConservative, {"2330"});
  OrderHubServer server(&backend, path);
  CHECK(server.Start());
  backend.OnBook("2330", Book({{9990, 100}}, {{10100, 100}}, Us(10, 0, 0)));  // continuous

  int fd = ConnectClient(path);
  CHECK(fd >= 0);
  CHECK(WriteFrame(fd, EncodeOrderSubmit(Order("k-1", Side::kBuy, 10000, 1000))));

  OrderMessage m;
  CHECK(ReadMsg(fd, &m) && m.kind == OrderMsgKind::kAck && m.ack.id == "k-1" && m.ack.ok);

  // A trade strictly through 100.00 fills the resting 1000 at its limit.
  backend.OnTrade("2330", Trade{9900, 1000, Us(10, 0, 5), false});
  CHECK(ReadMsg(fd, &m) && m.kind == OrderMsgKind::kFill && m.fill.id == "k-1" &&
        m.fill.shares == 1000 && m.fill.price == 10000);

  // Odd-lot board is rejected.
  CHECK(WriteFrame(fd, EncodeOrderSubmit({"odd", "2330", Market::kTse, Board::kOddLot, Side::kBuy,
                                          "Cash", "ROD", 10000, 500})));
  CHECK(ReadMsg(fd, &m) && m.kind == OrderMsgKind::kAck && !m.ack.ok);

  ::close(fd);
  server.Stop();
}

void TestClosingAuction() {
  std::string path = "/tmp/kairos-test-simhub-a-" + std::to_string(::getpid()) + ".sock";
  SimOrderBackend backend(FillMode::kConservative, {"2330"});
  OrderHubServer server(&backend, path);
  CHECK(server.Start());
  backend.OnTrade("2330", Trade{10000, 10, Us(10, 0, 0), false});           // reference := 100.00
  backend.OnBook("2330", Book({{10000, 0}}, {{10100, 0}}, Us(13, 25, 0)));  // closing window opens

  int fd = ConnectClient(path);
  CHECK(fd >= 0);
  CHECK(WriteFrame(fd, EncodeOrderSubmit(Order("buy", Side::kBuy, 10000, 1000))));
  CHECK(WriteFrame(fd, EncodeOrderSubmit(Order("sell", Side::kSell, 10000, 1000))));

  OrderMessage m;
  CHECK(ReadMsg(fd, &m) && m.kind == OrderMsgKind::kAck && m.ack.ok);  // buy ack
  CHECK(ReadMsg(fd, &m) && m.kind == OrderMsgKind::kAck && m.ack.ok);  // sell ack

  // Match at 13:30 (cross 100.00 == reference -> no delay): both fill 1000@100.00.
  backend.OnBook("2330", Book({{10000, 0}}, {{10100, 0}}, Us(13, 30, 0)));
  long buy_sh = 0, sell_sh = 0;
  for (int i = 0; i < 2; ++i) {
    if (!ReadMsg(fd, &m) || m.kind != OrderMsgKind::kFill) break;
    CHECK(m.fill.price == 10000);
    if (m.fill.id == "buy") buy_sh += m.fill.shares;
    if (m.fill.id == "sell") sell_sh += m.fill.shares;
  }
  CHECK(buy_sh == 1000 && sell_sh == 1000);

  ::close(fd);
  server.Stop();
}

void TestReconnectChurn() {
  std::string path = "/tmp/kairos-test-simhub-r-" + std::to_string(::getpid()) + ".sock";
  SimOrderBackend backend(FillMode::kConservative, {"2330"});
  OrderHubServer server(&backend, path);
  CHECK(server.Start());
  backend.OnBook("2330", Book({{9990, 100}}, {{10100, 100}}, Us(10, 0, 0)));
  for (int i = 0; i < 30; ++i) {
    int c = ConnectClient(path);
    CHECK(c >= 0);
    if (c < 0) break;
    WriteFrame(c, EncodeOrderSubmit(Order("churn", Side::kBuy, 10000, 1000)));
    ::close(c);
  }
  server.Stop();
}

}  // namespace

int main() {
  TestContinuous();
  TestClosingAuction();
  TestReconnectChurn();

  if (g_failures == 0) {
    std::printf("test_sim_hub_server: OK\n");
    return 0;
  }
  std::printf("test_sim_hub_server: FAILED %d check(s)\n", g_failures);
  return 1;
}
