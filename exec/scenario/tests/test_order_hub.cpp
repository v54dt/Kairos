// Self-test for OrderHub routing: client submit/cancel -> backend, and backend
// ack/fill/cancel -> the owning client. Stub backend + fake send; no sockets.

#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "order_codec.h"
#include "order_hub.h"
#include "order_journal.h"

using namespace kairos::exec;

static int g_failures = 0;

static std::vector<std::string> ReadLines(const std::string& path) {
  std::vector<std::string> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) out.push_back(line);
  return out;
}

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

    // A fill that crosses a cancel-ack in flight, then arrives after the trader
    // has exited, must still be journaled: the cancel-ack keeps the id's meta so
    // the residual fill stays nameable (not silently dropped -> re-buy on restart).
    const std::string name5 = std::string("2412-Buy-") + JournalDayUtc8();
    const std::string path5 = JournalPath(dir, name5);
    std::remove(path5.c_str());
    OrderSubmitMsg os5{"k9-5", "2412", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash",
                       "ROD",  10000,  1000};
    Feed(h2, 10, EncodeOrderSubmit(os5));
    b2.FireFill("k9-5", Fill{700, 10000});  // 700 routed to the live trader
    Feed(h2, 10, EncodeOrderCancel({"k9-5"}));
    b2.FireCancel("k9-5", true);            // cancel-acked with 300 still working
    h2.OnClientDisconnect(10);              // trader exits before the crossing fill lands
    b2.FireFill("k9-5", Fill{300, 10000});  // residual post-cancel fill
    auto fills5 = ReadJournalFills(path5);
    CHECK(fills5.size() == 1);
    CHECK(fills5[0].shares == 300);

    h2.Stop();
    std::remove(path3.c_str());
    std::remove(path4.c_str());
    std::remove(path5.c_str());
    std::remove(path.c_str());
    std::remove(path2.c_str());
    ::rmdir(dir.c_str());
  }

  // --- Order-flow audit stream: every hub event lands as exactly one well-formed
  // --- line in hub-orders-<day>.jsonl, distinct from the run-state fill journal. ---
  {
    const std::string dir = "/tmp/kairos-hub-flow-" + std::to_string(::getpid());
    StubBackend bf;
    std::vector<std::pair<int, OrderMessage>> sentf;
    auto sendf = [&](int c, const std::vector<std::uint8_t>& bytes) {
      OrderMessage m;
      if (DecodeOrder(bytes.data(), bytes.size(), &m)) sentf.push_back({c, m});
    };
    OrderHub::RiskConfig risk;
    risk.journal_dir = dir;  // order_flow_journal defaults on
    OrderHub hf(&bf, sendf, risk);
    CHECK(hf.Start());

    const std::string fpath = JournalPath(dir, "hub-orders-" + JournalDayUtc8());
    const std::string rpath = JournalPath(dir, std::string("2330-Buy-") + JournalDayUtc8());
    std::remove(fpath.c_str());
    std::remove(rpath.c_str());

    OrderSubmitMsg os{"kf-1", "2330", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash",
                      "ROD",  92500,  1000};
    Feed(hf, 11, EncodeOrderSubmit(os));
    bf.FireAck("kf-1", true, "");
    bf.FireFill("kf-1", Fill{1000, 92500});  // routed fill: unroutable=0
    Feed(hf, 11, EncodeOrderCancel({"kf-1"}));
    bf.FireCancel("kf-1", true);

    auto lines = ReadLines(fpath);
    CHECK(lines.size() == 5);
    CHECK(JournalJsonStr(lines[0], "type", "") == "submit");
    CHECK(JournalJsonStr(lines[0], "id", "") == "kf-1");
    CHECK(JournalJsonStr(lines[0], "prefix", "") == "kf");
    CHECK(JournalJsonStr(lines[0], "symbol", "") == "2330");
    CHECK(JournalJsonStr(lines[0], "side", "") == "Buy");
    CHECK(JournalJsonStr(lines[1], "type", "") == "ack");
    CHECK(JournalJsonInt(lines[1], "ok", -1) == 1);
    CHECK(JournalJsonStr(lines[2], "type", "") == "fill");
    CHECK(JournalJsonInt(lines[2], "unroutable", -1) == 0);
    CHECK(JournalJsonStr(lines[3], "type", "") == "cancel_req");
    CHECK(JournalJsonStr(lines[4], "type", "") == "cancel_ack");
    long prev = 0;  // timestamps are sane and non-decreasing
    for (const auto& l : lines) {
      long t = JournalJsonInt(l, "t", -1);
      CHECK(t >= prev);
      prev = t;
    }
    // The routed fill was NOT written into the replayed run-state journal.
    CHECK(ReadJournalFills(rpath).empty());

    // A fill on a departed client is flagged unroutable=1 in the same stream.
    OrderSubmitMsg os2{"kf-2", "0050", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash",
                       "ROD",  10000,  2000};
    Feed(hf, 12, EncodeOrderSubmit(os2));
    hf.OnClientDisconnect(12);
    bf.FireFill("kf-2", Fill{2000, 10000});
    auto lines2 = ReadLines(fpath);
    CHECK(lines2.size() == 7);
    CHECK(JournalJsonStr(lines2[6], "type", "") == "fill");
    CHECK(JournalJsonStr(lines2[6], "id", "") == "kf-2");
    CHECK(JournalJsonInt(lines2[6], "unroutable", -1) == 1);

    hf.Stop();
    std::remove(fpath.c_str());
    std::remove(JournalPath(dir, std::string("0050-Buy-") + JournalDayUtc8()).c_str());
    ::rmdir(dir.c_str());
  }

  // --- order_flow_journal=false writes NO audit file, yet orders still route. ---
  {
    const std::string dir = "/tmp/kairos-hub-flowoff-" + std::to_string(::getpid());
    StubBackend bo;
    std::vector<std::pair<int, OrderMessage>> sento;
    auto sendo = [&](int c, const std::vector<std::uint8_t>& bytes) {
      OrderMessage m;
      if (DecodeOrder(bytes.data(), bytes.size(), &m)) sento.push_back({c, m});
    };
    OrderHub::RiskConfig risk;
    risk.journal_dir = dir;
    risk.order_flow_journal = false;
    OrderHub ho(&bo, sendo, risk);
    CHECK(ho.Start());
    const std::string fpath = JournalPath(dir, "hub-orders-" + JournalDayUtc8());
    std::remove(fpath.c_str());
    OrderSubmitMsg os{"kx-1", "2330", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash",
                      "ROD",  92500,  1000};
    Feed(ho, 13, EncodeOrderSubmit(os));
    bo.FireAck("kx-1", true, "");
    CHECK(!sento.empty());            // order routed
    CHECK(ReadLines(fpath).empty());  // but no audit file
    ho.Stop();
    std::remove(fpath.c_str());
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

  // --- Trading-day rollover: crossing the UTC+8 day boundary resets the account
  // --- day-realized notional, the per-symbol collar reference, and the id->meta
  // --- map, so yesterday's state never leaks into today. The boundary is the
  // --- shared UTC+8 helper (host TZ irrelevant); forced here for determinism. ---
  {
    StubBackend b;
    std::vector<std::pair<int, OrderMessage>> sd;
    auto send = [&](int c, const std::vector<std::uint8_t>& bytes) {
      OrderMessage m;
      if (DecodeOrder(bytes.data(), bytes.size(), &m)) sd.push_back({c, m});
    };
    OrderHub::RiskConfig risk;
    risk.price_collar_pct = 10;                    // reject a price >10% from the last fill
    risk.max_account_notional_cents = 35'000'000;  // realized + open + new, account-wide
    OrderHub hub(&b, send, risk);
    CHECK(hub.Start());

    // Day 1: a full fill books day-realized notional and anchors the collar ref.
    hub.SetTradingDayForTest(20260710);
    OrderSubmitMsg d1{"k1-1", "2330", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash",
                      "ROD",  10000,  1000};  // NT$100.00 x 1000 = NT$100,000 realized
    Feed(hub, 1, EncodeOrderSubmit(d1));
    b.FireAck("k1-1", true, "");
    b.FireFill("k1-1", Fill{1000, 10000});  // route closes: realized 10M, open back to 0
    // Same-day: a probe whose notional needs day-realized to breach the cap is rejected.
    OrderSubmitMsg probe{"k1-2", "2330", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash",
                         "ROD",  10000,  3000};  // 30M; 10M realized + 30M > 35M cap
    Feed(hub, 1, EncodeOrderSubmit(probe));
    CHECK(!sd.empty() && sd.back().second.kind == OrderMsgKind::kAck && !sd.back().second.ack.ok);

    // Day 2: the next submit crosses the boundary; day-realized clears, so the same
    // probe now fits (0 realized + 30M <= 35M) and is forwarded to the backend.
    hub.SetTradingDayForTest(20260711);
    OrderSubmitMsg d2{"k2-1", "2330", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash",
                      "ROD",  10000,  3000};
    Feed(hub, 1, EncodeOrderSubmit(d2));
    CHECK(b.submits.back().id == "k2-1");     // admitted: day-realized was reset
    CHECK(hub.OrderMetaCountForTest() == 1);  // yesterday's meta cleared, only k2-1 remains
    b.FireFill("k2-1", Fill{1000, 10000});    // re-anchor the collar ref at 100.00

    // Same day 2: a wild price is collar-rejected against the fresh reference.
    OrderSubmitMsg wild{"k2-2", "2330", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash",
                        "ROD",  50000,  100};  // 500.00 vs 100.00 ref
    Feed(hub, 1, EncodeOrderSubmit(wild));
    CHECK(!sd.back().second.ack.ok);

    // Day 3: rollover clears the collar reference, so the same wild price is admitted.
    hub.SetTradingDayForTest(20260712);
    OrderSubmitMsg d3{"k3-1", "2330", Market::kTse, Board::kRoundLot, Side::kBuy, "Cash",
                      "ROD",  50000,  100};
    Feed(hub, 1, EncodeOrderSubmit(d3));
    CHECK(b.submits.back().id == "k3-1");     // admitted: collar reference was reset
    CHECK(hub.OrderMetaCountForTest() == 1);  // meta reset again, only k3-1 remains
    hub.Stop();
  }

  if (g_failures == 0) {
    std::printf("test_order_hub: OK\n");
    return 0;
  }
  std::printf("test_order_hub: FAILED %d check(s)\n", g_failures);
  return 1;
}
