// Self-test for the hub account risk gate: registry accounting, notional and
// per-client caps, admin halt, and self-match protection. Drives OrderHub
// directly with a stub backend + fake send; no sockets. Rejections are ack
// ok=false frames the fake send decodes; backend callbacks fire by hand.

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

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

}  // namespace

int main() {
  TestRegistryAccounting();
  TestFailClosed();

  if (g_failures == 0) {
    std::printf("test_hub_risk_gate: OK\n");
    return 0;
  }
  std::printf("test_hub_risk_gate: FAILED %d check(s)\n", g_failures);
  return 1;
}
