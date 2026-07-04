// Self-test for socket-path resolution + UDS framing. No broker; framing is
// exercised over an in-process socketpair. Also covers the additive trade
// callback: with a trade callback set, Trade frames are delivered; without one,
// the live path is unchanged (Trade frames are ignored, quotes still delivered).

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "kairos.capnp.h"
#include "socket_path.h"
#include "uds_frame.h"
#include "uds_quote_client.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

static void TestSocketPath() {
  const char* q = "kairos-quotes.sock";
  // explicit env wins
  CHECK(ResolveSock("/run/k.sock", "/run/user/1001", "/run/user/1001", q) == "/run/k.sock");
  // XDG preferred over /run/user
  CHECK(ResolveSock(nullptr, "/run/user/1001", "/run/user/1001", q) ==
        "/run/user/1001/kairos-quotes.sock");
  // /run/user used when XDG is unset
  CHECK(ResolveSock(nullptr, nullptr, "/run/user/1001", q) == "/run/user/1001/kairos-quotes.sock");
  // nothing usable -> empty (never /tmp)
  CHECK(ResolveSock(nullptr, nullptr, "", q).empty());
  CHECK(ResolveSock("", "", "", q).empty());
  // empty values skip to the next candidate
  CHECK(ResolveSock("", nullptr, "/run/user/1001", q) == "/run/user/1001/kairos-quotes.sock");
}

static void TestFraming() {
  int sv[2];
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

  std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5, 0xff, 0x00, 0x42};
  CHECK(WriteFrame(sv[0], payload));
  std::vector<std::uint8_t> empty;
  CHECK(WriteFrame(sv[0], empty));

  std::vector<std::uint8_t> out;
  CHECK(ReadFrame(sv[1], &out) == 1);
  CHECK(out == payload);
  CHECK(ReadFrame(sv[1], &out) == 1);
  CHECK(out.empty());

  ::close(sv[0]);  // writer gone -> clean EOF on the reader
  CHECK(ReadFrame(sv[1], &out) == 0);
  ::close(sv[1]);
}

static std::vector<std::uint8_t> Flatten(capnp::MessageBuilder& msg) {
  auto flat = capnp::messageToFlatArray(msg);
  auto bytes = flat.asBytes();
  return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

static std::vector<std::uint8_t> BuildTradeFrame() {
  capnp::MallocMessageBuilder msg;
  auto tr = msg.initRoot<Envelope>().initTrade();
  tr.setSymbol("2330");
  tr.setPriceMantissa(58050);
  tr.setPriceScale(2);
  tr.setVolume(9);
  tr.setTradeTsUs(1700000000000000LL);
  return Flatten(msg);
}

static std::vector<std::uint8_t> BuildQuoteFrame() {
  capnp::MallocMessageBuilder msg;
  auto q = msg.initRoot<Envelope>().initQuote();
  q.setSymbol("2330");
  auto bids = q.initBids(1);
  bids[0].setPriceMantissa(58000);
  bids[0].setPriceScale(2);
  bids[0].setVolume(100);
  return Flatten(msg);
}

// One quote server: bind/listen, accept one client, read its subscribe frame,
// then send a Trade frame followed by a Quote frame.
static void ServeOnce(const std::string& path, std::atomic<bool>* ready) {
  int lfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  ::unlink(path.c_str());
  ::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  ::listen(lfd, 1);
  *ready = true;
  int c = ::accept(lfd, nullptr, nullptr);
  if (c >= 0) {
    std::vector<std::uint8_t> sub;
    ReadFrame(c, &sub);  // subscribe
    WriteFrame(c, BuildTradeFrame());
    WriteFrame(c, BuildQuoteFrame());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ::close(c);
  }
  ::close(lfd);
  ::unlink(path.c_str());
}

// Runs a client against ServeOnce; returns (trades, quotes) delivered.
static void TestTradeCallback() {
  for (bool with_trade : {true, false}) {
    std::string path = "/tmp/kairos-test-uds-" + std::to_string(::getpid()) + "-" +
                       std::to_string(with_trade) + ".sock";
    std::atomic<bool> ready{false};
    std::thread server([&] { ServeOnce(path, &ready); });
    while (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));

    std::atomic<int> trades{0}, quotes{0};
    UdsQuoteClient client(path, {"2330"});
    client.SetCallback([&](const std::string&, const TopOfBook&) { ++quotes; });
    if (with_trade)
      client.SetTradeCallback([&](const std::string& s, const kairos::exec::Trade& t) {
        if (s == "2330" && t.price == 58050 && t.volume == 9) ++trades;
      });
    client.Start();
    for (int i = 0; i < 200 && quotes.load() == 0; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    client.Stop();
    server.join();

    CHECK(quotes.load() == 1);  // quote always delivered
    if (with_trade) {
      CHECK(trades.load() == 1);  // trade delivered when a callback is set
    } else {
      CHECK(trades.load() == 0);  // no callback -> Trade frame ignored, live path unchanged
    }
  }
}

int main() {
  TestSocketPath();
  TestFraming();
  TestTradeCallback();
  if (g_failures == 0) {
    std::printf("test_uds: OK\n");
    return 0;
  }
  std::printf("test_uds: FAILED %d check(s)\n", g_failures);
  return 1;
}
