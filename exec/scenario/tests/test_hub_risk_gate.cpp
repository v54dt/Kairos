// Self-test for the hub account risk gate: registry accounting, notional and
// per-client caps, admin halt, and self-match protection. Drives OrderHub
// directly with a stub backend + fake send; no sockets. Rejections are ack
// ok=false frames the fake send decodes; backend callbacks fire by hand.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hub_status.h"
#include "order_codec.h"
#include "order_hub.h"
#include "risk_gate.h"
#include "test_check.h"

using namespace kairos::exec;

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
  hub.DrainForwardedForTest();  // forwarding is async now; wait for the backend to see it
}

void Cancel(OrderHub& hub, int client, const std::string& id) {
  auto b = EncodeOrderCancel({id});
  hub.OnClientMessage(client, b.data(), b.size());
  hub.DrainForwardedForTest();  // wait for the forwarder to issue the broker cancel
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
  expect_reject(Order("hi-shares", "2330", Side::kBuy, 10000, kMaxTwStockShares + 1));
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

// A pathological share count that would overflow price*shares int64 must be
// rejected (not admitted via a wrapped-negative notional), and must not corrupt
// the shared account aggregate for the orders that follow.
void TestNotionalOverflowRejected() {
  StubBackend backend;
  Sink sink;
  OrderHub::RiskConfig cfg;
  cfg.max_account_notional_cents = 30000000;  // 300,000 TWD
  OrderHub hub(&backend, sink.Fn(), cfg);
  CHECK(hub.Start());

  // price 100.00 * 1e15 shares = 1e19c, which wraps int64 negative if unguarded.
  Feed(hub, 7, Order("overflow", "2330", Side::kBuy, 10000, 1000000000000000L));
  CHECK(backend.submits.empty());  // rejected, never forwarded
  CHECK(sink.Last().kind == OrderMsgKind::kAck && !sink.Last().ack.ok);
  CHECK(hub.CaptureStatus().account_open_notional_cents == 0);  // aggregate uncorrupted

  // The cap still holds for the next order: 5,000,000c (>> 300,000 cap) is blocked.
  Feed(hub, 7, Order("over-cap", "2330", Side::kBuy, 50000, 10000));
  CHECK(backend.submits.empty());
  CHECK(sink.Last().kind == OrderMsgKind::kAck && !sink.Last().ack.ok);

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

// ---- self-match protection (155-1-5) -----------------------------------

void TestSelfMatch() {
  StubBackend backend;
  Sink sink;
  OrderHub hub(&backend, sink.Fn());  // self_match_protection on by default
  CHECK(hub.Start());

  // Client 7 rests a buy 2330@100.00; client 9's crossing sell at/below 100.00
  // would self-match across the shared account -> rejected, not forwarded.
  Feed(hub, 7, Order("A", "2330", Side::kBuy, 10000, 1000));
  CHECK(backend.submits.size() == 1);
  Feed(hub, 9, Order("B", "2330", Side::kSell, 10000, 1000));  // sell <= open buy -> cross
  CHECK(backend.submits.size() == 1);                          // B not forwarded
  CHECK(sink.Last().kind == OrderMsgKind::kAck && !sink.Last().ack.ok);
  CHECK(sink.Last().ack.error_message.find("self-match risk vs open order A") != std::string::npos);

  // The resting buy A is untouched (still open).
  CHECK(hub.CaptureStatus().account_open_notional_cents == 10000000);

  // A non-crossing opposite-side sell (above the open buy) routes normally.
  Feed(hub, 9, Order("C", "2330", Side::kSell, 10100, 1000));  // sell > open buy -> no cross
  CHECK(backend.submits.size() == 2);

  // A same-symbol same-side buy routes (only opposite side is a self-match).
  Feed(hub, 9, Order("D", "2330", Side::kBuy, 9900, 1000));
  CHECK(backend.submits.size() == 3);

  // A different symbol opposite side never self-matches.
  Feed(hub, 9, Order("E", "1101", Side::kSell, 5000, 1000));
  CHECK(backend.submits.size() == 4);

  // Once the resting buy A is cancelled it no longer blocks a crossing sell:
  // closed orders are dropped from the self-match scan.
  Cancel(hub, 7, "A");
  backend.FireCancel("A", true);
  Feed(hub, 9, Order("F", "2330", Side::kSell, 10000, 1000));  // would cross A, but A is gone
  CHECK(backend.submits.size() == 5);

  hub.Stop();
}

// ---- cancels are never blocked -----------------------------------------

void TestCancelsNeverBlocked() {
  StubBackend backend;
  Sink sink;
  OrderHub::RiskConfig cfg;
  cfg.max_account_notional_cents = 1;  // every submit is over the cap
  cfg.max_open_orders_per_client = 1;  // and over the count limit
  OrderHub hub(&backend, sink.Fn(), cfg);
  CHECK(hub.Start());

  // Over every cap: a submit is rejected, but a cancel still forwards.
  Feed(hub, 7, Order("x", "2330", Side::kBuy, 10000, 1000));
  CHECK(backend.submits.empty());
  Cancel(hub, 7, "x");
  CHECK(backend.cancels.size() == 1);

  // Under admin halt: a submit is rejected, but a cancel still forwards.
  hub.SetAdminHalt(true);
  Feed(hub, 7, Order("y", "2330", Side::kBuy, 10000, 1000));
  CHECK(backend.submits.empty());
  Cancel(hub, 7, "y");
  CHECK(backend.cancels.size() == 2);

  hub.Stop();
}

// ---- max order size (c) ------------------------------------------------

void TestMaxOrderSize() {
  StubBackend backend;
  Sink sink;
  OrderHub::RiskConfig cfg;
  cfg.max_order_shares = 1000;
  OrderHub hub(&backend, sink.Fn(), cfg);
  CHECK(hub.Start());

  Feed(hub, 7, Order("s1", "2330", Side::kBuy, 10000, 1000));  // at cap -> routes
  CHECK(backend.submits.size() == 1);
  Feed(hub, 7, Order("s2", "2330", Side::kBuy, 10000, 1001));  // over cap -> reject
  CHECK(backend.submits.size() == 1);                          // not forwarded
  CHECK(sink.Last().kind == OrderMsgKind::kAck && !sink.Last().ack.ok);
  CHECK(sink.Last().ack.error_message.find("exceeds max") != std::string::npos);
  hub.Stop();

  // Disabled (0): a huge share count within kMaxTwStockShares routes.
  StubBackend backend2;
  Sink sink2;
  OrderHub hub2(&backend2, sink2.Fn());  // max_order_shares defaults to 0
  CHECK(hub2.Start());
  Feed(hub2, 7, Order("big", "2330", Side::kBuy, 10000, 2000000));
  CHECK(backend2.submits.size() == 1);
  hub2.Stop();
}

// ---- duplicate-order detection (b) -------------------------------------

void TestDuplicateOrder() {
  StubBackend backend;
  Sink sink;
  OrderHub::RiskConfig cfg;
  cfg.dup_order_window_ms = 60000;
  OrderHub hub(&backend, sink.Fn(), cfg);
  CHECK(hub.Start());
  hub.SetMonoMsForTest(1000);

  Feed(hub, 7, Order("A", "2330", Side::kBuy, 10000, 10000));  // first -> routes
  CHECK(backend.submits.size() == 1);
  Feed(hub, 7, Order("B", "2330", Side::kBuy, 10000, 10000));  // identical fields -> reject
  CHECK(backend.submits.size() == 1);                          // not forwarded
  CHECK(sink.Last().kind == OrderMsgKind::kAck && !sink.Last().ack.ok);
  CHECK(sink.Last().ack.error_message.find("duplicate order") != std::string::npos);

  // A different price is not a duplicate -> routes.
  Feed(hub, 7, Order("C", "2330", Side::kBuy, 10100, 10000));
  CHECK(backend.submits.size() == 2);

  // A fill and a cancel between different orders must not create a false dup.
  backend.FireAck("A", true, "");
  backend.FireFill("A", Fill{10000, 10000});  // A fully fills -> terminal, dup entry cleared
  backend.FireCancel("C", true);              // C cancelled -> terminal, dup entry cleared
  Feed(hub, 7, Order("D", "2330", Side::kBuy, 500, 10000));  // different shares -> routes
  CHECK(backend.submits.size() == 3);

  // A reached a terminal state (full fill), so an identical re-order within the
  // window is a legitimate sequential order, not a runaway loop -> routes.
  Feed(hub, 7, Order("A2", "2330", Side::kBuy, 10000, 10000));
  CHECK(backend.submits.size() == 4);

  // Outside the window the same fields route again (ring pruned).
  hub.SetMonoMsForTest(1000 + 60001);
  Feed(hub, 7, Order("E", "2330", Side::kBuy, 10000, 10000));  // == A fields, but window elapsed
  CHECK(backend.submits.size() == 5);
  hub.Stop();

  // Disabled (0): two identical submits both route.
  StubBackend backend2;
  Sink sink2;
  OrderHub hub2(&backend2, sink2.Fn());  // dup_order_window_ms defaults to 0
  CHECK(hub2.Start());
  Feed(hub2, 7, Order("x1", "2330", Side::kBuy, 10000, 10000));
  Feed(hub2, 7, Order("x2", "2330", Side::kBuy, 10000, 10000));
  CHECK(backend2.submits.size() == 2);
  hub2.Stop();
}

// ---- price collar (a) --------------------------------------------------

void TestPriceCollar() {
  StubBackend backend;
  Sink sink;
  OrderHub::RiskConfig cfg;
  cfg.price_collar_pct = 10;
  OrderHub hub(&backend, sink.Fn(), cfg);
  CHECK(hub.Start());

  // Cold start: no fill yet on 2330 -> collar inactive -> routes.
  Feed(hub, 7, Order("cs", "2330", Side::kBuy, 10000, 1000));
  CHECK(backend.submits.size() == 1);

  // Establish the reference from the account's own fill at 100.00.
  backend.FireAck("cs", true, "");
  backend.FireFill("cs", Fill{1000, 10000});

  Feed(hub, 7, Order("in", "2330", Side::kBuy, 10500, 1000));  // +5% -> routes
  CHECK(backend.submits.size() == 2);

  Feed(hub, 7, Order("hi", "2330", Side::kBuy, 12000, 1000));  // +20% -> reject
  CHECK(backend.submits.size() == 2);
  CHECK(sink.Last().kind == OrderMsgKind::kAck && !sink.Last().ack.ok);
  CHECK(sink.Last().ack.error_message.find("deviates") != std::string::npos);

  Feed(hub, 7, Order("lo", "2330", Side::kBuy, 8000, 1000));  // -20% -> reject
  CHECK(backend.submits.size() == 2);

  // Wrong-scale mantissa (price*100), still below the absolute ceiling -> reject.
  Feed(hub, 7, Order("scale", "2330", Side::kBuy, 1000000, 1000));
  CHECK(backend.submits.size() == 2);
  CHECK(sink.Last().kind == OrderMsgKind::kAck && !sink.Last().ack.ok);
  hub.Stop();

  // Disabled (0): an off-band price routes (no collar, no reference recorded).
  StubBackend backend2;
  Sink sink2;
  OrderHub hub2(&backend2, sink2.Fn());  // price_collar_pct defaults to 0
  CHECK(hub2.Start());
  Feed(hub2, 7, Order("d1", "2330", Side::kBuy, 10000, 1000));
  backend2.FireAck("d1", true, "");
  backend2.FireFill("d1", Fill{1000, 10000});
  Feed(hub2, 7, Order("d2", "2330", Side::kBuy, 5000000, 1000));  // wild but < ceiling
  CHECK(backend2.submits.size() == 2);
  hub2.Stop();
}

// ---- collar reference resets at the trading-day boundary ----------------

void TestCollarDayBoundaryReset() {
  StubBackend backend;
  Sink sink;
  OrderHub::RiskConfig cfg;
  cfg.price_collar_pct = 10;
  OrderHub hub(&backend, sink.Fn(), cfg);
  CHECK(hub.Start());
  hub.SetTradingDayForTest(20260704);

  // Day N: establish the reference from the account's own fill at 100.00.
  Feed(hub, 7, Order("d1", "2330", Side::kBuy, 10000, 1000));
  backend.FireAck("d1", true, "");
  backend.FireFill("d1", Fill{1000, 10000});

  // Same day: a +20% price is off-band against that reference -> rejected.
  Feed(hub, 7, Order("gap", "2330", Side::kBuy, 12000, 1000));
  CHECK(backend.submits.size() == 1);
  CHECK(sink.Last().kind == OrderMsgKind::kAck && !sink.Last().ack.ok);

  // Day N+1: the reference is cleared at the rollover, so the first order is a
  // cold start (collar inactive) instead of being wedged by yesterday's fill.
  hub.SetTradingDayForTest(20260705);
  Feed(hub, 7, Order("open", "2330", Side::kBuy, 12000, 1000));
  CHECK(backend.submits.size() == 2);
  hub.Stop();
}

// ---- all guards disabled: PR #96 behavior unchanged --------------------

void TestGuardsAllDisabled() {
  StubBackend backend;
  Sink sink;
  OrderHub hub(&backend, sink.Fn());  // max_order_shares / dup_window / collar all 0
  CHECK(hub.Start());

  Feed(hub, 7, Order("big", "2330", Side::kBuy, 10000, 2000000));  // large -> routes
  Feed(hub, 7, Order("id1", "2330", Side::kBuy, 10000, 1000));     // identical fields, diff ids
  Feed(hub, 7, Order("id2", "2330", Side::kBuy, 10000, 1000));     // -> both route
  Feed(hub, 7, Order("off", "1101", Side::kBuy, 9000000, 1000));   // wild price -> routes
  CHECK(backend.submits.size() == 4);
  hub.Stop();
}

// ---- cancels never blocked with every guard enabled --------------------

void TestCancelsNeverBlockedUnderGuards() {
  StubBackend backend;
  Sink sink;
  OrderHub::RiskConfig cfg;
  cfg.max_order_shares = 1;
  cfg.dup_order_window_ms = 1000000;
  cfg.price_collar_pct = 1;
  OrderHub hub(&backend, sink.Fn(), cfg);
  CHECK(hub.Start());

  Feed(hub, 7, Order("z", "2330", Side::kBuy, 10000, 1000));  // tripped by max_order_shares
  CHECK(backend.submits.empty());
  Cancel(hub, 7, "z");
  CHECK(backend.cancels.size() == 1);
  Cancel(hub, 7, "other");
  CHECK(backend.cancels.size() == 2);
  hub.Stop();
}

// ---- all guards enabled: each trips its own submit, none forwarded ------

void TestGuardsCombinedFailClosed() {
  StubBackend backend;
  Sink sink;
  OrderHub::RiskConfig cfg;
  cfg.max_order_shares = 1000;
  cfg.dup_order_window_ms = 60000;
  cfg.price_collar_pct = 10;
  OrderHub hub(&backend, sink.Fn(), cfg);
  CHECK(hub.Start());
  hub.SetMonoMsForTest(1000);

  Feed(hub, 7, Order("seed", "2330", Side::kBuy, 10000, 1000));  // establishes the collar ref
  CHECK(backend.submits.size() == 1);
  backend.FireAck("seed", true, "");
  backend.FireFill("seed", Fill{1000, 10000});  // terminal: seed leaves the dup ring

  // A still-live order for the (b) duplicate check to trip against.
  Feed(hub, 7, Order("live", "2330", Side::kBuy, 10000, 1000));
  CHECK(backend.submits.size() == 2);
  backend.FireAck("live", true, "");  // acked, unfilled: stays in the dup ring

  std::size_t base = backend.submits.size();
  Feed(hub, 7, Order("g1", "2330", Side::kBuy, 10000, 1001));  // (c) oversize
  Feed(hub, 7, Order("g2", "2330", Side::kBuy, 20000, 100));   // (a) off-price
  Feed(hub, 7, Order("g3", "2330", Side::kBuy, 10000, 1000));  // (b) duplicate of the live order
  CHECK(backend.submits.size() == base);  // none forwarded: all three fail-closed
  hub.Stop();
}

// ======================================================================
// Direct RiskGate unit layer: exercises the money-guard with no hub, no
// backend, no sockets. Each clause gets an admit + a reject; the precedence
// matrix and the reject-string pins lock the first-match order and wording.
// ======================================================================

// Holds the containers the RiskStateView refers to so a view can be built by
// hand, then mutated between evaluations.
struct GateFixture {
  std::unordered_map<std::string, Route> routes;
  std::unordered_set<std::string> open_ids;
  std::unordered_map<std::string, Cents> last_fill;
  std::deque<DupEntry> dup_ring;
  std::int64_t account_open = 0;
  std::int64_t account_realized = 0;
  int client_open_orders = 0;
  std::int64_t client_open_notional = 0;
  long now_ms = 0;

  RiskStateView View() {
    return RiskStateView{routes,       open_ids,         last_fill,          dup_ring,
                         account_open, account_realized, client_open_orders, client_open_notional,
                         now_ms};
  }
  void AddOpen(const std::string& id, const std::string& sym, Side side, Cents price, long shares) {
    routes[id] = Route{7, shares, true, false, sym, side, price};
    open_ids.insert(id);
  }
};

std::optional<std::string> Eval(const RiskConfig& cfg, GateFixture& fx, const OrderSubmitMsg& o) {
  RiskGate gate(cfg);
  RiskStateView view = fx.View();
  return gate.Evaluate(o, view);
}

// ---- one admit + one reject per clause ---------------------------------

void TestGateInvalidFields() {
  RiskConfig cfg;
  GateFixture fx;
  const OrderSubmitMsg bad[] = {
      Order("", "2330", Side::kBuy, 10000, 1000),                         // empty id
      Order("id", "", Side::kBuy, 10000, 1000),                           // empty symbol
      Order("id", "2330", Side::kBuy, 10000, 0),                          // zero shares
      Order("id", "2330", Side::kBuy, 10000, -5),                         // negative shares
      Order("id", "2330", Side::kBuy, 10000, kMaxTwStockShares + 1),      // over share ceiling
      Order("id", "2330", Side::kBuy, 0, 1000),                           // zero price
      Order("id", "2330", Side::kBuy, -1, 1000),                          // negative price
      Order("id", "2330", Side::kBuy, kMaxTwStockPriceCents + 1, 1000)};  // over price ceiling
  for (const auto& o : bad) {
    auto r = Eval(cfg, fx, o);
    CHECK(r && *r == "invalid order fields");
  }
  CHECK(!Eval(cfg, fx, Order("ok", "2330", Side::kBuy, 10000, 1000)));  // admit
}

void TestGateDuplicateLiveId() {
  RiskConfig cfg;
  GateFixture fx;
  fx.AddOpen("D", "2330", Side::kBuy, 10000, 1000);  // live, not closed
  auto r = Eval(cfg, fx, Order("D", "2330", Side::kBuy, 10000, 1000));
  CHECK(r && *r == "duplicate live order id");

  // A closed route with the same id is not a live duplicate -> admit.
  fx.routes["D"].closed = true;
  fx.open_ids.erase("D");
  CHECK(!Eval(cfg, fx, Order("D", "2330", Side::kBuy, 10000, 1000)));
}

void TestGateMaxOrderShares() {
  RiskConfig cfg;
  cfg.max_order_shares = 1000;
  GateFixture fx;
  auto r = Eval(cfg, fx, Order("s", "2330", Side::kBuy, 10000, 1001));
  CHECK(r && *r == "order size 1001 exceeds max 1000");
  CHECK(!Eval(cfg, fx, Order("s", "2330", Side::kBuy, 10000, 1000)));  // at cap -> admit

  RiskConfig off;  // disabled (0): a huge count within the ceiling admits
  CHECK(!Eval(off, fx, Order("s", "2330", Side::kBuy, 10000, 2000000)));
}

void TestGatePriceCollar() {
  RiskConfig cfg;
  cfg.price_collar_pct = 10;
  GateFixture fx;
  fx.last_fill["2330"] = 10000;  // reference 100.00
  auto r = Eval(cfg, fx, Order("hi", "2330", Side::kBuy, 12000, 1000));
  CHECK(r && *r == "price 120.00 deviates >10% from last fill 100.00 (fat-finger?)");
  CHECK(!Eval(cfg, fx, Order("in", "2330", Side::kBuy, 10500, 1000)));  // +5% -> admit

  GateFixture cold;  // no reference yet -> collar inactive -> admit even off-band
  CHECK(!Eval(cfg, cold, Order("cs", "2330", Side::kBuy, 12000, 1000)));

  RiskConfig off;  // disabled (0): off-band admits
  CHECK(!Eval(off, fx, Order("d", "2330", Side::kBuy, 5000000, 1000)));
}

void TestGateDuplicateWindow() {
  RiskConfig cfg;
  cfg.dup_order_window_ms = 60000;
  GateFixture fx;
  fx.now_ms = 1000;
  fx.dup_ring.push_back({"live", "2330", Side::kBuy, 1000, 10000, 1000});
  auto r = Eval(cfg, fx, Order("dup", "2330", Side::kBuy, 10000, 1000));
  CHECK(r && *r == "duplicate order within 60000ms (suspected runaway loop)");
  CHECK(!Eval(cfg, fx, Order("p", "2330", Side::kBuy, 10100, 1000)));  // different price -> admit

  // Outside the window the ring is pruned in place -> admit, ring emptied.
  fx.now_ms = 1000 + 60001;
  CHECK(!Eval(cfg, fx, Order("late", "2330", Side::kBuy, 10000, 1000)));
  CHECK(fx.dup_ring.empty());
}

void TestGateAccountNotionalCap() {
  RiskConfig cfg;
  cfg.max_account_notional_cents = 30000000;
  GateFixture fx;
  fx.account_open = 20000000;
  auto r = Eval(cfg, fx, Order("c", "2330", Side::kBuy, 15000, 1000));  // +15M -> 35M > 30M
  CHECK(r && *r == "account notional cap exceeded");
  CHECK(!Eval(cfg, fx, Order("d", "2330", Side::kBuy, 10000, 500)));  // +5M -> 25M -> admit
}

void TestGatePerClientOpenOrders() {
  RiskConfig cfg;
  cfg.max_open_orders_per_client = 2;
  GateFixture fx;
  fx.client_open_orders = 2;
  auto r = Eval(cfg, fx, Order("o", "2330", Side::kBuy, 10000, 100));
  CHECK(r && *r == "per-client open-order limit exceeded");
  fx.client_open_orders = 1;
  CHECK(!Eval(cfg, fx, Order("o", "2330", Side::kBuy, 10000, 100)));  // 1+1 <= 2 -> admit
}

void TestGatePerClientNotional() {
  RiskConfig cfg;
  cfg.max_open_notional_per_client_cents = 25000000;
  GateFixture fx;
  fx.client_open_notional = 20000000;
  auto r = Eval(cfg, fx, Order("p", "2330", Side::kBuy, 20000, 1000));  // +20M -> 40M > 25M
  CHECK(r && *r == "per-client open-notional limit exceeded");
  fx.client_open_notional = 10000000;
  CHECK(!Eval(cfg, fx, Order("p", "2330", Side::kBuy, 10000, 1000)));  // +10M -> 20M -> admit
}

void TestGateSelfMatch() {
  RiskConfig cfg;  // self_match_protection on by default
  GateFixture fx;
  fx.AddOpen("A", "2330", Side::kSell, 10000, 1000);                    // resting sell 100.00
  auto r = Eval(cfg, fx, Order("B", "2330", Side::kBuy, 10000, 1000));  // buy >= sell -> cross
  CHECK(r && *r == "self-match risk vs open order A");
  CHECK(!Eval(cfg, fx, Order("C", "2330", Side::kBuy, 9900, 1000)));    // buy < sell -> no cross
  CHECK(!Eval(cfg, fx, Order("D", "2330", Side::kSell, 10000, 1000)));  // same side -> no cross
  CHECK(!Eval(cfg, fx, Order("E", "1101", Side::kBuy, 10000, 1000)));   // other symbol -> no cross

  RiskConfig off;  // protection disabled: a crossing order admits
  off.self_match_protection = false;
  CHECK(!Eval(off, fx, Order("F", "2330", Side::kBuy, 10000, 1000)));
}

// ---- precedence matrix (first-match wins) ------------------------------
// Recorded against the pre-refactor hub (unmodified code, this worktree) with
// every co-violated clause active. Each expected string below is the exact
// reject the original inline chain produced for the same multi-violation input.

void TestGatePrecedenceMatrix() {
  // P1: invalid fields (share ceiling) beats oversize (clause 1 > 3).
  {
    RiskConfig cfg;
    cfg.max_order_shares = 1000;
    GateFixture fx;
    auto r = Eval(cfg, fx, Order("p1", "2330", Side::kBuy, 10000, kMaxTwStockShares + 1));
    CHECK(r && *r == "invalid order fields");
  }
  // P2: duplicate live id beats oversize + off-band (clause 2 > 3, 4).
  {
    RiskConfig cfg;
    cfg.max_order_shares = 1000;
    cfg.price_collar_pct = 10;
    GateFixture fx;
    fx.AddOpen("D", "2330", Side::kBuy, 10000, 1000);
    fx.last_fill["2330"] = 10000;
    auto r = Eval(cfg, fx, Order("D", "2330", Side::kBuy, 12000, 2000));
    CHECK(r && *r == "duplicate live order id");
  }
  // P3: oversize beats off-band + account cap (clause 3 > 4, 6).
  {
    RiskConfig cfg;
    cfg.max_order_shares = 1000;
    cfg.price_collar_pct = 10;
    cfg.max_account_notional_cents = 25000000;
    GateFixture fx;
    fx.last_fill["2330"] = 10000;
    fx.account_open = 20000000;
    auto r = Eval(cfg, fx, Order("p3", "2330", Side::kBuy, 12000, 2000));
    CHECK(r && *r == "order size 2000 exceeds max 1000");
  }
  // P4: off-band price beats duplicate-window (clause 4 > 5).
  {
    RiskConfig cfg;
    cfg.price_collar_pct = 10;
    cfg.dup_order_window_ms = 60000;
    GateFixture fx;
    fx.last_fill["2330"] = 10000;
    fx.now_ms = 1000;
    fx.dup_ring.push_back({"live", "2330", Side::kBuy, 1000, 12000, 1000});
    auto r = Eval(cfg, fx, Order("p4", "2330", Side::kBuy, 12000, 1000));
    CHECK(r && *r == "price 120.00 deviates >10% from last fill 100.00 (fat-finger?)");
  }
  // P5: duplicate-window beats account + per-client caps (clause 5 > 6, 7, 8).
  {
    RiskConfig cfg;
    cfg.dup_order_window_ms = 60000;
    cfg.max_account_notional_cents = 25000000;
    cfg.max_open_orders_per_client = 2;
    cfg.max_open_notional_per_client_cents = 25000000;
    GateFixture fx;
    fx.now_ms = 1000;
    fx.dup_ring.push_back({"live", "2330", Side::kBuy, 1000, 10000, 1000});
    fx.account_open = 20000000;
    fx.client_open_orders = 2;
    fx.client_open_notional = 20000000;
    auto r = Eval(cfg, fx, Order("p5", "2330", Side::kBuy, 10000, 1000));
    CHECK(r && *r == "duplicate order within 60000ms (suspected runaway loop)");
  }
  // P6: account cap beats per-client caps + self-match (clause 6 > 7, 8, 9).
  {
    RiskConfig cfg;
    cfg.max_account_notional_cents = 25000000;
    cfg.max_open_orders_per_client = 2;
    cfg.max_open_notional_per_client_cents = 25000000;
    GateFixture fx;
    fx.AddOpen("rs", "2330", Side::kSell, 10000, 1000);  // crossing sell for self-match
    fx.account_open = 20100000;
    fx.client_open_orders = 2;
    fx.client_open_notional = 20100000;
    auto r = Eval(cfg, fx, Order("p6", "2330", Side::kBuy, 10000, 1000));
    CHECK(r && *r == "account notional cap exceeded");
  }
  // P7: per-client open-order beats per-client notional + self-match, account
  // cap disabled (clause 7 > 8, 9).
  {
    RiskConfig cfg;
    cfg.max_account_notional_cents = 0;
    cfg.max_open_orders_per_client = 2;
    cfg.max_open_notional_per_client_cents = 25000000;
    GateFixture fx;
    fx.AddOpen("rs", "2330", Side::kSell, 10000, 1000);
    fx.account_open = 20100000;
    fx.client_open_orders = 2;
    fx.client_open_notional = 20100000;
    auto r = Eval(cfg, fx, Order("p7", "2330", Side::kBuy, 10000, 1000));
    CHECK(r && *r == "per-client open-order limit exceeded");
  }
}

// ---- reject-string pins: every gate message, byte-for-byte --------------
// A future wording change must edit this test too, making it a conscious act.

void TestGateRejectStringPins() {
  {
    RiskConfig cfg;
    GateFixture fx;
    CHECK(*Eval(cfg, fx, Order("", "2330", Side::kBuy, 10000, 1000)) == "invalid order fields");
  }
  {
    RiskConfig cfg;
    GateFixture fx;
    fx.AddOpen("D", "2330", Side::kBuy, 10000, 1000);
    CHECK(*Eval(cfg, fx, Order("D", "2330", Side::kBuy, 10000, 1000)) == "duplicate live order id");
  }
  {
    RiskConfig cfg;
    cfg.max_order_shares = 1000;
    GateFixture fx;
    CHECK(*Eval(cfg, fx, Order("s", "2330", Side::kBuy, 10000, 1001)) ==
          "order size 1001 exceeds max 1000");
  }
  {
    RiskConfig cfg;
    cfg.price_collar_pct = 10;
    GateFixture fx;
    fx.last_fill["2330"] = 10000;
    CHECK(*Eval(cfg, fx, Order("hi", "2330", Side::kBuy, 12000, 1000)) ==
          "price 120.00 deviates >10% from last fill 100.00 (fat-finger?)");
  }
  {
    RiskConfig cfg;
    cfg.dup_order_window_ms = 60000;
    GateFixture fx;
    fx.now_ms = 1000;
    fx.dup_ring.push_back({"live", "2330", Side::kBuy, 1000, 10000, 1000});
    CHECK(*Eval(cfg, fx, Order("dup", "2330", Side::kBuy, 10000, 1000)) ==
          "duplicate order within 60000ms (suspected runaway loop)");
  }
  {
    RiskConfig cfg;
    cfg.max_account_notional_cents = 1;
    GateFixture fx;
    CHECK(*Eval(cfg, fx, Order("a", "2330", Side::kBuy, 10000, 1000)) ==
          "account notional cap exceeded");
  }
  {
    RiskConfig cfg;
    cfg.max_open_orders_per_client = 1;
    GateFixture fx;
    fx.client_open_orders = 1;
    CHECK(*Eval(cfg, fx, Order("o", "2330", Side::kBuy, 10000, 100)) ==
          "per-client open-order limit exceeded");
  }
  {
    RiskConfig cfg;
    cfg.max_open_notional_per_client_cents = 1;
    GateFixture fx;
    CHECK(*Eval(cfg, fx, Order("p", "2330", Side::kBuy, 10000, 1000)) ==
          "per-client open-notional limit exceeded");
  }
  {
    RiskConfig cfg;
    GateFixture fx;
    fx.AddOpen("A", "2330", Side::kSell, 10000, 1000);
    CHECK(*Eval(cfg, fx, Order("B", "2330", Side::kBuy, 10000, 1000)) ==
          "self-match risk vs open order A");
  }
}

// ---- client-stat parity pin: a rejected submit does not touch client stats
// The gate reads a non-inserting client-stat snapshot, so a reject leaves the
// admit-path accounting untouched (the hub owns and mutates clients_ only on
// admit). Pins that the delegation kept the admit-side accounting identical.

void TestRejectLeavesClientStatsUntouched() {
  StubBackend backend;
  Sink sink;
  OrderHub::RiskConfig cfg;
  cfg.max_open_orders_per_client = 2;
  cfg.max_order_shares = 1000;
  OrderHub hub(&backend, sink.Fn(), cfg);
  CHECK(hub.Start());

  hub.OnClientConnect(7);
  Feed(hub, 7, Order("k7-1", "2330", Side::kBuy, 10000, 1000));  // admit: submitted 1, 10M open
  HubStatus s = hub.CaptureStatus();
  CHECK(s.client_count == 1);
  CHECK(s.account_open_notional_cents == 10000000);
  CHECK(s.clients.size() == 1 && s.clients[0].submitted == 1);

  Feed(hub, 7, Order("k7-2", "2330", Side::kBuy, 10000, 2000));  // reject: oversize
  s = hub.CaptureStatus();
  CHECK(s.client_count == 1);                                   // no new client, no change
  CHECK(s.account_open_notional_cents == 10000000);             // aggregate untouched by the reject
  CHECK(s.clients.size() == 1 && s.clients[0].submitted == 1);  // submitted not bumped by a reject

  hub.Stop();
}

}  // namespace

int main() {
  TestRegistryAccounting();
  TestFailClosed();
  TestAccountNotionalCap();
  TestNotionalOverflowRejected();
  TestPerClientCaps();
  TestAdminHalt();
  TestHaltFile();
  TestSelfMatch();
  TestCancelsNeverBlocked();
  TestMaxOrderSize();
  TestDuplicateOrder();
  TestPriceCollar();
  TestCollarDayBoundaryReset();
  TestGuardsAllDisabled();
  TestCancelsNeverBlockedUnderGuards();
  TestGuardsCombinedFailClosed();

  TestGateInvalidFields();
  TestGateDuplicateLiveId();
  TestGateMaxOrderShares();
  TestGatePriceCollar();
  TestGateDuplicateWindow();
  TestGateAccountNotionalCap();
  TestGatePerClientOpenOrders();
  TestGatePerClientNotional();
  TestGateSelfMatch();
  TestGatePrecedenceMatrix();
  TestGateRejectStringPins();
  TestRejectLeavesClientStatsUntouched();

  if (g_failures == 0) {
    std::printf("test_hub_risk_gate: OK\n");
    return 0;
  }
  std::printf("test_hub_risk_gate: FAILED %d check(s)\n", g_failures);
  return 1;
}
