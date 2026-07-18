// Integration test: OrderHubServer over a real UDS socket with a PaperOrderBackend.
// A client submits and must receive the routed ack + fill. No broker.

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "order_backend.h"
#include "order_codec.h"
#include "order_hub_server.h"
#include "order_journal.h"
#include "test_check.h"
#include "uds_frame.h"

using namespace kairos::exec;

static std::vector<std::string> ReadLines(const std::string& path) {
  std::vector<std::string> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) out.push_back(line);
  return out;
}

static int ConnectClient(const std::string& path) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  timeval tv{2, 0};  // a missing reply fails instead of hanging
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

int main() {
  std::string path = "/tmp/kairos-test-hub-" + std::to_string(::getpid()) + ".sock";
  PaperOrderBackend backend;
  OrderHubServer server(&backend, path);
  CHECK(server.Start());

  int fd = ConnectClient(path);
  CHECK(fd >= 0);
  if (fd < 0) {
    server.Stop();
    std::printf("test_order_hub_server: FAILED (connect)\n");
    return 1;
  }

  OrderSubmitMsg m{"k-1", "2330", Market::kTse, Board::kOddLot, Side::kBuy, "Cash",
                   "ROD", 92500,  1000};
  CHECK(WriteFrame(fd, EncodeOrderSubmit(m)));

  // paper acks then fills -> expect both routed back to this client (a leading
  // forwarded frame precedes them, so drain up to three frames)
  bool got_ack = false, got_fill = false;
  for (int i = 0; i < 3; ++i) {
    std::vector<std::uint8_t> frame;
    if (ReadFrame(fd, &frame) != 1) break;
    OrderMessage om;
    if (!DecodeOrder(frame.data(), frame.size(), &om)) continue;
    if (om.kind == OrderMsgKind::kAck) {
      got_ack = om.ack.id == "k-1" && om.ack.ok;
    } else if (om.kind == OrderMsgKind::kFill) {
      got_fill = om.fill.id == "k-1" && om.fill.shares == 1000 && om.fill.price == 92500;
    }
  }
  CHECK(got_ack);
  CHECK(got_fill);

  // reconnect churn: many sequential connect/submit/close must not crash. fd
  // reuse used to reassign a still-joinable std::thread -> std::terminate.
  for (int i = 0; i < 30; ++i) {
    int c = ConnectClient(path);
    CHECK(c >= 0);
    if (c < 0) break;
    WriteFrame(c, EncodeOrderSubmit({"churn", "2330", Market::kTse, Board::kOddLot, Side::kBuy,
                                     "Cash", "ROD", 10000, 1000}));
    ::close(c);
  }

  ::close(fd);
  server.Stop();

  // --- End-to-end order-flow audit demo over a real socket: submit -> ack ->
  // --- fill -> cancel produces a hub-orders file that reconstructs the sequence. ---
  {
    const std::string dir = "/tmp/kairos-hub-e2e-" + std::to_string(::getpid());
    const std::string sock = "/tmp/kairos-test-hube2e-" + std::to_string(::getpid()) + ".sock";
    const std::string fpath = JournalPath(dir, "hub-orders-" + JournalDayUtc8());
    std::remove(fpath.c_str());

    PaperOrderBackend pb;
    OrderHub::RiskConfig risk;
    risk.journal_dir = dir;
    OrderHubServer srv(&pb, sock, risk);
    CHECK(srv.Start());

    int c = ConnectClient(sock);
    CHECK(c >= 0);
    if (c >= 0) {
      OrderSubmitMsg m{"k-e2e", "2330", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash",
                       "ROD",   92500,  1000};
      CHECK(WriteFrame(c, EncodeOrderSubmit(m)));
      for (int i = 0; i < 3; ++i) {  // drain forwarded + ack + fill
        std::vector<std::uint8_t> frame;
        if (ReadFrame(c, &frame) != 1) break;
      }
      CHECK(WriteFrame(c, EncodeOrderCancel({"k-e2e"})));
      std::vector<std::string> lines;
      // submit, forwarded, ack, fill, cancel_req, cancel_ack -> 6 lines.
      for (int i = 0; i < 200 && lines.size() < 6; ++i) {  // bounded wait for async writes
        lines = ReadLines(fpath);
        if (lines.size() < 6) ::usleep(10000);
      }
      CHECK(lines.size() == 6);
      std::printf("--- %s ---\n", fpath.c_str());
      for (const auto& l : lines) std::printf("%s\n", l.c_str());
      std::printf("--- end ---\n");
      if (lines.size() == 6) {
        CHECK(JournalJsonStr(lines[0], "type", "") == "submit");
        CHECK(JournalJsonStr(lines[1], "type", "") == "forwarded");
        CHECK(JournalJsonStr(lines[2], "type", "") == "ack");
        CHECK(JournalJsonStr(lines[3], "type", "") == "fill");
        CHECK(JournalJsonStr(lines[4], "type", "") == "cancel_req");
        CHECK(JournalJsonStr(lines[5], "type", "") == "cancel_ack");
        long prev = 0;
        for (const auto& l : lines) {
          long t = JournalJsonInt(l, "t", -1);
          CHECK(t >= prev);
          prev = t;
        }
      }
      ::close(c);
    }
    srv.Stop();
    std::remove(fpath.c_str());
    ::rmdir(dir.c_str());
  }

  if (g_failures == 0) {
    std::printf("test_order_hub_server: OK\n");
    return 0;
  }
  std::printf("test_order_hub_server: FAILED %d check(s)\n", g_failures);
  return 1;
}
