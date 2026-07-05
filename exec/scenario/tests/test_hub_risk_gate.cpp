// Self-test for the hub account risk gate: registry accounting, notional and
// per-client caps, admin halt, and self-match protection. Drives OrderHub
// directly with a stub backend + fake send; no sockets. Rejections are ack
// ok=false frames the fake send decodes; backend callbacks fire by hand.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "hub_status.h"
#include "order_codec.h"
#include "order_hub.h"

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

class StubBackend : public OrderBackend {
 public:
  bool Connect() override {
    connected = true;
    return true;
  }
  void Disconnect() override { connected = false; }
  bool IsConnected() const override { return connected; }
  void Submit(const OrderSubmitMsg& o) override { submits.push_back(o); }
  void Cancel(const std::string& id) override { cancels.push_back(id); }

  void FireAck(const std::string& id, bool ok, const std::string& e = "") {
    if (on_ack_) on_ack_(id, ok, e);
  }
  void FireFill(const std::string& id, const Fill& f) {
    if (on_fill_) on_fill_(id, f);
  }
  void FireCancel(const std::string& id, bool ok) {
    if (on_cancel_) on_cancel_(id, ok);
  }

  bool connected = false;
  std::vector<OrderSubmitMsg> submits;
  std::vector<std::string> cancels;
};

// Fake send that records every hub->client frame the gate emits.
struct Sink {
  std::vector<std::pair<int, OrderMessage>> sent;
  OrderHub::SendFn Fn() {
    return [this](int c, const std::vector<std::uint8_t>& b) {
      OrderMessage m;
      if (DecodeOrder(b.data(), b.size(), &m)) sent.push_back({c, m});
    };
  }
  const OrderMessage& Last() const { return sent.back().second; }
};

OrderSubmitMsg Order(const std::string& id, const std::string& sym, Side side, Cents price,
                     long shares) {
  return {id, sym, Market::kTse, Board::kRoundLot, side, "Cash", "ROD", price, shares};
}

void Feed(OrderHub& hub, int client, const OrderSubmitMsg& o) {
  auto b = EncodeOrderSubmit(o);
  hub.OnClientMessage(client, b.data(), b.size());
}

void Cancel(OrderHub& hub, int client, const std::string& id) {
  auto b = EncodeOrderCancel({id});
  hub.OnClientMessage(client, b.data(), b.size());
}

// ---- registry accounting -----------------------------------------------

void TestRegistryAccounting() {
  StubBackend backend;
  Sink sink;
  OrderHub hub(&backend, sink.Fn());
  CHECK(hub.Start());

  Feed(hub, 7, Order("k7-1", "2330", Side::kBuy, 10000, 1000));  // 10,000,000c open
  Feed(hub, 7, Order("k7-2", "2330", Side::kBuy, 20000, 500));   // 10,000,000c open
  CHECK(backend.submits.size() == 2);
  HubStatus s = hub.CaptureStatus();
  CHECK(s.account_open_notional_cents == 20000000);
  CHECK(s.account_day_realized_cents == 0);

  backend.FireAck("k7-1", true, "");
  backend.FireFill("k7-1", Fill{400, 10000});  // partial: open -4,000,000c, realized +4,000,000c
  s = hub.CaptureStatus();
  CHECK(s.account_open_notional_cents == 16000000);
  CHECK(s.account_day_realized_cents == 4000000);

  backend.FireFill("k7-1", Fill{600, 10000});  // full: closes k7-1, open -6,000,000c
  s = hub.CaptureStatus();
  CHECK(s.account_open_notional_cents == 10000000);  // only k7-2 remains open
  CHECK(s.account_day_realized_cents == 10000000);

  backend.FireCancel("k7-2", true);  // cancel frees k7-2's reserved open
  s = hub.CaptureStatus();
  CHECK(s.account_open_notional_cents == 0);

  // Disconnect frees a client's still-open reserved notional from the account.
  Feed(hub, 8, Order("k8-1", "1101", Side::kSell, 5000, 1000));  // 5,000,000c open
  s = hub.CaptureStatus();
  CHECK(s.account_open_notional_cents == 5000000);
  hub.OnClientDisconnect(8);
  s = hub.CaptureStatus();
  CHECK(s.account_open_notional_cents == 0);

  // Day boundary resets the realized total on the next submit.
  hub.SetTradingDayForTest(20260101);
  Feed(hub, 9, Order("k9-1", "2330", Side::kBuy, 10000, 100));
  backend.FireFill("k9-1", Fill{100, 10000});  // realized 1,000,000c on day 20260101
  s = hub.CaptureStatus();
  CHECK(s.account_day_realized_cents == 1000000);
  hub.SetTradingDayForTest(20260102);
  Feed(hub, 9, Order("k9-2", "2330", Side::kBuy, 10000, 100));  // new day -> realized reset
  s = hub.CaptureStatus();
  CHECK(s.account_day_realized_cents == 0);

  hub.Stop();
}

// ---- fail-closed --------------------------------------------------------

void TestFailClosed() {
  StubBackend backend;
  Sink sink;
  OrderHub hub(&backend, sink.Fn());
  CHECK(hub.Start());

  auto expect_reject = [&](const OrderSubmitMsg& o) {
    std::size_t before = backend.submits.size();
    std::size_t sent_before = sink.sent.size();
    Feed(hub, 7, o);
    CHECK(backend.submits.size() == before);  // never forwarded
    CHECK(sink.sent.size() == sent_before + 1);
    CHECK(sink.Last().kind == OrderMsgKind::kAck && !sink.Last().ack.ok);
  };

  expect_reject(Order("bad-shares", "2330", Side::kBuy, 10000, 0));
  expect_reject(Order("bad-shares2", "2330", Side::kBuy, 10000, -5));
  expect_reject(Order("bad-price", "2330", Side::kBuy, 0, 1000));
  expect_reject(Order("bad-price2", "2330", Side::kBuy, -1, 1000));
  expect_reject(Order("hi-price", "2330", Side::kBuy, kMaxTwStockPriceCents + 1, 1000));
  expect_reject(Order("", "2330", Side::kBuy, 10000, 1000));
  expect_reject(Order("no-sym", "", Side::kBuy, 10000, 1000));

  // Duplicate live id: first admits, second is rejected and not forwarded.
  Feed(hub, 7, Order("dup", "2330", Side::kBuy, 10000, 1000));
  CHECK(backend.submits.size() == 1);
  expect_reject(Order("dup", "2330", Side::kBuy, 10000, 1000));

  // A malformed (undecodable) frame is silently dropped, not forwarded, no ack.
  std::size_t before = backend.submits.size();
  std::size_t sent_before = sink.sent.size();
  std::vector<std::uint8_t> junk = {9, 9, 9};
  hub.OnClientMessage(7, junk.data(), junk.size());
  CHECK(backend.submits.size() == before);
  CHECK(sink.sent.size() == sent_before);

  hub.Stop();
}

// ---- notional + per-client caps ----------------------------------------

void TestAccountNotionalCap() {
  StubBackend backend;
  Sink sink;
  OrderHub::RiskConfig cfg;
  cfg.max_account_notional_cents = 30000000;  // 300,000 TWD
  OrderHub hub(&backend, sink.Fn(), cfg);
  CHECK(hub.Start());

  Feed(hub, 7, Order("a", "2330", Side::kBuy, 10000, 1000));  // 10,000,000c open -> fits
  CHECK(backend.submits.size() == 1);
  Feed(hub, 7, Order("b", "2330", Side::kBuy, 10000, 1000));  // +10,000,000c -> 20,000,000c, fits
  CHECK(backend.submits.size() == 2);

  // Realized keeps counting toward the cap: fill "a" fully (10,000,000c realized).
  backend.FireAck("a", true, "");
  backend.FireFill("a", Fill{1000, 10000});
  // Now realized 10,000,000 + open 10,000,000 (b) = 20,000,000; a +15,000,000 order exceeds 30M.
  Feed(hub, 7, Order("c", "2330", Side::kBuy, 15000, 1000));  // 15,000,000c -> 35M > 30M cap
  CHECK(backend.submits.size() == 2);                         // not forwarded
  CHECK(sink.Last().kind == OrderMsgKind::kAck && !sink.Last().ack.ok);

  // A smaller order that still fits routes.
  Feed(hub, 7, Order("d", "2330", Side::kBuy, 10000, 500));  // 5,000,000c -> 25M <= 30M
  CHECK(backend.submits.size() == 3);

  hub.Stop();
}

void TestPerClientCaps() {
  StubBackend backend;
  Sink sink;
  OrderHub::RiskConfig cfg;
  cfg.max_open_orders_per_client = 2;
  cfg.max_open_notional_per_client_cents = 25000000;  // 250,000 TWD
  OrderHub hub(&backend, sink.Fn(), cfg);
  CHECK(hub.Start());

  Feed(hub, 7, Order("o1", "2330", Side::kBuy, 10000, 1000));  // 10M, 1 order
  Feed(hub, 7, Order("o2", "2330", Side::kBuy, 10000, 1000));  // 20M, 2 orders
  CHECK(backend.submits.size() == 2);
  Feed(hub, 7, Order("o3", "2330", Side::kBuy, 10000, 100));  // 3rd order over count limit
  CHECK(backend.submits.size() == 2);
  CHECK(sink.Last().kind == OrderMsgKind::kAck && !sink.Last().ack.ok);

  // A different client has its own budget and routes.
  Feed(hub, 9, Order("p1", "2330", Side::kBuy, 10000, 1000));
  CHECK(backend.submits.size() == 3);

  // Per-client notional limit: client 9's second order would exceed 25M.
  Feed(hub, 9, Order("p2", "2330", Side::kBuy, 20000, 1000));  // +20M -> 30M > 25M
  CHECK(backend.submits.size() == 3);
  CHECK(sink.Last().kind == OrderMsgKind::kAck && !sink.Last().ack.ok);

  hub.Stop();
}

// ---- admin halt kill switch --------------------------------------------

void TestAdminHalt() {
  StubBackend backend;
  Sink sink;
  OrderHub hub(&backend, sink.Fn());
  CHECK(hub.Start());

  hub.SetAdminHalt(true);
  CHECK(hub.IsHaltedNow());
  CHECK(hub.CaptureStatus().halted);
  Feed(hub, 7, Order("h1", "2330", Side::kBuy, 10000, 1000));
  CHECK(backend.submits.empty());  // not forwarded
  CHECK(sink.Last().kind == OrderMsgKind::kAck && !sink.Last().ack.ok);

  // A cancel still reaches the backend while halted (operator can flatten).
  Cancel(hub, 7, "some-open-id");
  CHECK(backend.cancels.size() == 1);

  hub.SetAdminHalt(true);  // idempotent
  CHECK(hub.IsHaltedNow());
  hub.SetAdminHalt(false);
  CHECK(!hub.IsHaltedNow());
  CHECK(!hub.CaptureStatus().halted);
  Feed(hub, 7, Order("h2", "2330", Side::kBuy, 10000, 1000));
  CHECK(backend.submits.size() == 1);  // routes again

  hub.Stop();
}

void TestHaltFile() {
  const std::string path =
      "/tmp/kairos-hub-halt-test-" + std::to_string(::getpid());  // scratch sentinel
  ::setenv("KAIROS_HUB_HALT", path.c_str(), 1);
  ::unlink(path.c_str());
  CHECK(HubHaltPath() == path);

  StubBackend backend;
  Sink sink;
  OrderHub::RiskConfig cfg;
  cfg.halt_file_path = HubHaltPath();
  OrderHub hub(&backend, sink.Fn(), cfg);
  CHECK(hub.Start());

  Feed(hub, 7, Order("f0", "2330", Side::kBuy, 10000, 1000));
  CHECK(backend.submits.size() == 1);  // no file: routes

  std::FILE* f = std::fopen(path.c_str(), "we");  // create sentinel -> halt observed
  if (f) std::fclose(f);
  CHECK(hub.IsHaltedNow());
  Feed(hub, 7, Order("f1", "2330", Side::kBuy, 10000, 1000));
  CHECK(backend.submits.size() == 1);  // rejected, not forwarded
  Cancel(hub, 7, "f0");
  CHECK(backend.cancels.size() == 1);  // cancel still forwarded while halted

  ::unlink(path.c_str());  // clear sentinel -> halt lifted
  CHECK(!hub.IsHaltedNow());
  Feed(hub, 7, Order("f2", "2330", Side::kBuy, 10000, 1000));
  CHECK(backend.submits.size() == 2);  // routes again

  hub.Stop();
  ::unsetenv("KAIROS_HUB_HALT");
}

}  // namespace

int main() {
  TestRegistryAccounting();
  TestFailClosed();
  TestAccountNotionalCap();
  TestPerClientCaps();
  TestAdminHalt();
  TestHaltFile();

  if (g_failures == 0) {
    std::printf("test_hub_risk_gate: OK\n");
    return 0;
  }
  std::printf("test_hub_risk_gate: FAILED %d check(s)\n", g_failures);
  return 1;
}
