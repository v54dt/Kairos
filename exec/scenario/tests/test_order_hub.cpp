// Self-test for OrderHub routing: client submit/cancel -> backend, and backend
// ack/fill/cancel -> the owning client. Stub backend + fake send; no sockets.

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

// Records submits/cancels and lets the test fire backend callbacks by hand.
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

  void FireAck(const std::string& id, bool ok, const std::string& e) {
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

void Feed(OrderHub& hub, int client, const std::vector<std::uint8_t>& bytes) {
  hub.OnClientMessage(client, bytes.data(), bytes.size());
}

}  // namespace

int main() {
  StubBackend backend;
  std::vector<std::pair<int, OrderMessage>> sent;
  auto send = [&](int client, const std::vector<std::uint8_t>& bytes) {
    OrderMessage m;
    if (DecodeOrder(bytes.data(), bytes.size(), &m)) sent.push_back({client, m});
  };

  OrderHub hub(&backend, send);
  CHECK(hub.Start());
  CHECK(backend.IsConnected());

  // client 7 submits -> backend gets the full spec, hub remembers the route
  OrderSubmitMsg s1{"k1-1", "2330", Market::kTse, Board::kOddLot, Side::kBuy, "Cash",
                    "ROD",  92500,  1000};
  Feed(hub, 7, EncodeOrderSubmit(s1));
  CHECK(backend.submits.size() == 1);
  CHECK(backend.submits[0].id == "k1-1");
  CHECK(backend.submits[0].symbol == "2330");
  CHECK(backend.submits[0].shares == 1000);

  // ack + fill for k1-1 route back to client 7
  backend.FireAck("k1-1", true, "");
  CHECK(sent.size() == 1);
  CHECK(sent[0].first == 7);
  CHECK(sent[0].second.kind == OrderMsgKind::kAck);
  CHECK(sent[0].second.ack.id == "k1-1");
  CHECK(sent[0].second.ack.ok);

  backend.FireFill("k1-1", Fill{1000, 92500});
  CHECK(sent.size() == 2);
  CHECK(sent[1].first == 7);
  CHECK(sent[1].second.kind == OrderMsgKind::kFill);
  CHECK(sent[1].second.fill.shares == 1000);
  CHECK(sent[1].second.fill.price == 92500);

  // a different client's order routes independently
  OrderSubmitMsg s2{"k2-1", "0050", Market::kTse, Board::kRoundLot, Side::kSell, "Cash",
                    "ROD",  10000,  2000};
  Feed(hub, 9, EncodeOrderSubmit(s2));
  backend.FireFill("k2-1", Fill{2000, 10000});
  CHECK(sent.size() == 3);
  CHECK(sent[2].first == 9);  // went to client 9, not 7
  CHECK(sent[2].second.fill.id == "k2-1");

  // cancel from client 7 -> backend, and the result routes back
  Feed(hub, 7, EncodeOrderCancel({"k1-1"}));
  CHECK(backend.cancels.size() == 1);
  CHECK(backend.cancels[0] == "k1-1");
  backend.FireCancel("k1-1", true);
  CHECK(sent.size() == 4);
  CHECK(sent[3].first == 7);
  CHECK(sent[3].second.kind == OrderMsgKind::kCancelResult);
  CHECK(sent[3].second.cancel_result.ok);

  // events for an unknown id are dropped (no route), and every unroutable callback
  // path is exercised (the hub logs them; nothing is sent, nothing crashes).
  backend.FireAck("ghost", true, "");
  backend.FireFill("ghost", Fill{1, 1});
  backend.FireCancel("ghost", true);
  CHECK(sent.size() == 4);

  // after a client disconnects, its routes are gone
  hub.OnClientDisconnect(7);
  backend.FireFill("k1-1", Fill{1, 1});
  CHECK(sent.size() == 4);  // not routed anywhere

  // malformed client frame is ignored
  std::vector<std::uint8_t> junk = {9, 9, 9};
  Feed(hub, 7, junk);
  CHECK(backend.submits.size() == 2);  // unchanged (s1, s2 only)

  hub.Stop();
  CHECK(!backend.IsConnected());

  if (g_failures == 0) {
    std::printf("test_order_hub: OK\n");
    return 0;
  }
  std::printf("test_order_hub: FAILED %d check(s)\n", g_failures);
  return 1;
}
