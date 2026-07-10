// Self-test for OrderHub routing: client submit/cancel -> backend, and backend
// ack/fill/cancel -> the owning client. Stub backend + fake send; no sockets.

#include <unistd.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "order_codec.h"
#include "order_hub.h"
#include "order_journal.h"

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

  // --- Unroutable-fill journaling: a fill on a departed client's still-open id
  // --- must land in the SAME per-(symbol,side,day) journal a restart replays. ---
  {
    const std::string dir = "/tmp/kairos-hub-journal-" + std::to_string(::getpid());
    StubBackend b2;
    std::vector<std::pair<int, OrderMessage>> sent2;
    auto send2 = [&](int c, const std::vector<std::uint8_t>& bytes) {
      OrderMessage m;
      if (DecodeOrder(bytes.data(), bytes.size(), &m)) sent2.push_back({c, m});
    };
    OrderHub::RiskConfig risk;
    risk.journal_dir = dir;
    OrderHub h2(&b2, send2, risk);
    CHECK(h2.Start());

    const std::string name = std::string("2330-Buy-") + JournalDayUtc8();
    const std::string path = JournalPath(dir, name);
    std::remove(path.c_str());

    OrderSubmitMsg os{"k9-1", "2330", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash",
                      "ROD",  92500,  1000};
    Feed(h2, 5, EncodeOrderSubmit(os));

    // Routable fill (client 5 still connected): the hub writes NOTHING; the fill
    // is the trader's own to journal.
    b2.FireFill("k9-1", Fill{400, 92500});
    CHECK(ReadJournalFills(path).empty());

    // Client 5 goes away with 600 shares still open. A residual fill on that id is
    // now unroutable -> the hub journals exactly one line.
    h2.OnClientDisconnect(5);
    b2.FireFill("k9-1", Fill{600, 92500});
    auto fills = ReadJournalFills(path);
    CHECK(fills.size() == 1);
    CHECK(fills[0].shares == 600);
    CHECK(fills[0].price == 92500);

    // A fully-filled id is forgotten on its terminal fill; a later stray fill on
    // it (after disconnect) cannot be named and is NOT journaled.
    const std::string name2 = std::string("0050-Sell-") + JournalDayUtc8();
    const std::string path2 = JournalPath(dir, name2);
    std::remove(path2.c_str());
    OrderSubmitMsg os2{"k9-2", "0050", Market::kTse, Board::kRoundLot, Side::kSell, "Cash",
                       "ROD",  10000,  2000};
    Feed(h2, 6, EncodeOrderSubmit(os2));
    b2.FireFill("k9-2", Fill{2000, 10000});  // full fill -> terminal -> forgotten
    h2.OnClientDisconnect(6);
    b2.FireFill("k9-2", Fill{1, 10000});  // stray, meta gone
    CHECK(ReadJournalFills(path2).empty());

    // A broker that RE-DELIVERS the same fill on an orphaned id (standby/resync
    // feed replay) must not double-journal: the unroutable path caps at the order
    // size and forgets the id once fully accounted, so a restart replays it once.
    const std::string name3 = std::string("2454-Buy-") + JournalDayUtc8();
    const std::string path3 = JournalPath(dir, name3);
    std::remove(path3.c_str());
    OrderSubmitMsg os3{"k9-3", "2454", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash",
                       "ROD",  50000,  1000};
    Feed(h2, 8, EncodeOrderSubmit(os3));
    h2.OnClientDisconnect(8);
    b2.FireFill("k9-3", Fill{1000, 50000});  // first delivery -> journaled
    b2.FireFill("k9-3", Fill{1000, 50000});  // redelivery -> dropped (fully accounted)
    b2.FireFill("k9-3", Fill{1000, 50000});  // redelivery -> dropped
    auto fills3 = ReadJournalFills(path3);
    CHECK(fills3.size() == 1);
    CHECK(fills3[0].shares == 1000);

    // An unroutable fill larger than the order's unaccounted quantity is capped
    // at that quantity, never over-counting past the order size.
    const std::string name4 = std::string("2603-Buy-") + JournalDayUtc8();
    const std::string path4 = JournalPath(dir, name4);
    std::remove(path4.c_str());
    OrderSubmitMsg os4{"k9-4", "2603", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash",
                       "ROD",  30000,  500};
    Feed(h2, 9, EncodeOrderSubmit(os4));
    h2.OnClientDisconnect(9);
    b2.FireFill("k9-4", Fill{800, 30000});  // over-delivery -> capped at 500
    auto fills4 = ReadJournalFills(path4);
    CHECK(fills4.size() == 1);
    CHECK(fills4[0].shares == 500);

    h2.Stop();
    std::remove(path3.c_str());
    std::remove(path4.c_str());
    std::remove(path.c_str());
    std::remove(path2.c_str());
    ::rmdir(dir.c_str());
  }

  // --- An unopenable journal dir must not stall or crash the hub: the write
  // --- fails loudly and a following live order still routes. ---
  {
    StubBackend b3;
    std::vector<std::pair<int, OrderMessage>> sent3;
    auto send3 = [&](int c, const std::vector<std::uint8_t>& bytes) {
      OrderMessage m;
      if (DecodeOrder(bytes.data(), bytes.size(), &m)) sent3.push_back({c, m});
    };
    OrderHub::RiskConfig risk;
    risk.journal_dir = "/proc/nonexistent/cannot-create";  // unwritable
    OrderHub h3(&b3, send3, risk);
    CHECK(h3.Start());

    OrderSubmitMsg os{"k8-1", "2330", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash",
                      "ROD",  92500,  1000};
    Feed(h3, 3, EncodeOrderSubmit(os));
    h3.OnClientDisconnect(3);
    b3.FireFill("k8-1", Fill{1000, 92500});  // unroutable + unwritable: loud log, no crash

    // The hub keeps serving: a fresh order still routes and its fill comes back.
    OrderSubmitMsg os2{"k8-2", "0050", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash",
                       "ROD",  10000,  2000};
    Feed(h3, 4, EncodeOrderSubmit(os2));
    CHECK(b3.submits.size() == 2);
    b3.FireFill("k8-2", Fill{2000, 10000});
    CHECK(!sent3.empty());
    CHECK(sent3.back().first == 4);
    h3.Stop();
  }

  // --- The id->{symbol,side} map is hard-bounded: driving more never-terminal
  // --- submits than the cap leaves it capped (no unbounded day-long growth). ---
  {
    StubBackend b4;
    auto send4 = [&](int, const std::vector<std::uint8_t>&) {};
    OrderHub::RiskConfig risk;
    risk.self_match_protection = false;  // all same-side, keep the scan cheap under load
    OrderHub h4(&b4, send4, risk);
    CHECK(h4.Start());
    const long n = static_cast<long>(OrderHub::kOrderMetaCap) + 1000;
    for (long i = 0; i < n; ++i) {
      OrderSubmitMsg os{"kcap-" + std::to_string(i),
                        "2330",
                        Market::kTse,
                        Board::kRoundLot,
                        Side::kBuy,
                        "Cash",
                        "ROD",
                        92500,
                        1000};
      Feed(h4, 2, EncodeOrderSubmit(os));
    }
    CHECK(h4.OrderMetaCountForTest() == OrderHub::kOrderMetaCap);
    h4.Stop();
  }

  if (g_failures == 0) {
    std::printf("test_order_hub: OK\n");
    return 0;
  }
  std::printf("test_order_hub: FAILED %d check(s)\n", g_failures);
  return 1;
}
