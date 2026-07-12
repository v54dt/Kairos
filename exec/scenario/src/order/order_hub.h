#ifndef KAIROS_EXEC_ORDER_HUB_H_
#define KAIROS_EXEC_ORDER_HUB_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hub_status.h"
#include "order_backend.h"
#include "risk_gate.h"  // RiskConfig, Route, DupEntry, RiskGate
#include "scenario.h"   // Side
#include "tw_market.h"  // Cents

namespace kairos::exec {

// Routing core of the concords order hub: many scenario clients submit/cancel
// through one shared OrderBackend (one concords connection, one 1 req/s gate).
// Tracks which client owns each user_defined_id and routes the backend's
// ack/fill/cancel events back to that client. Socket-agnostic: the server
// injects a SendFn, so the routing is unit-testable without sockets.
class OrderHub {
 public:
  // Deliver a serialized OrderEnvelope to client `client`.
  using SendFn = std::function<void(int client, const std::vector<std::uint8_t>&)>;

  // The gate's account risk-gate limits live in risk_gate.h; keep the historic
  // OrderHub::RiskConfig spelling for every existing construction site.
  using RiskConfig = kairos::exec::RiskConfig;

  OrderHub(OrderBackend* backend, SendFn send);
  OrderHub(OrderBackend* backend, SendFn send, RiskConfig risk);

  bool Start();  // wire backend callbacks + connect
  void Stop();   // disconnect the backend

  // Register a client the moment its connection is accepted, so the status
  // snapshot counts connected-but-idle traders (before any submit).
  void OnClientConnect(int client);
  // Feed one client frame (a serialized OrderEnvelope). Malformed frames and
  // hub->client message kinds (ack/fill/...) are ignored.
  void OnClientMessage(int client, const std::uint8_t* data, std::size_t len);
  // Drop all routing for a client whose connection went away.
  void OnClientDisconnect(int client);

  // Read-only snapshot of hub + per-client state; safe to call off the hot path.
  HubStatus CaptureStatus() const;

  // Kill switch: while set, every new submit is rejected; cancels keep flowing.
  // Idempotent; effective immediately. Also driven by the halt sentinel file.
  void SetAdminHalt(bool halted);
  bool IsHaltedNow() const;

  // Force the trading day the day-notional reset compares against (tests only).
  void SetTradingDayForTest(long day);
  // Force the monotonic clock the duplicate-order window compares against (tests only).
  void SetMonoMsForTest(long ms);
  // Size of the surviving id->{symbol,side} map (tests only).
  std::size_t OrderMetaCountForTest() const;

  // Hard bound on the id->{symbol,side} map so it cannot grow unbounded across a
  // trading day; the oldest entry is FIFO-evicted once this many are tracked.
  static constexpr std::size_t kOrderMetaCap = 8192;

 private:
  // The (symbol, side) of one order, kept past its route so a fill that arrives
  // after the owning client is gone can still be named for the journal.
  // shares_accounted tracks how many of shares_total already reached the journal
  // (routed to the live trader, which journals its own, plus any the hub wrote),
  // so an unroutable fill is capped at the order size and a redelivery on a
  // fully-accounted id is not journaled twice.
  struct OrderMeta {
    std::string symbol;
    Side side = Side::kBuy;
    long shares_total = 0;
    long shares_accounted = 0;
  };

  // Lifetime aggregate for one connected client (fd), for the status snapshot.
  struct ClientStats {
    std::string prefix;
    long pid = 0;
    long submitted = 0;
    long filled = 0;
    long cancelled = 0;
    long last_activity_us = 0;
    std::int64_t open_notional = 0;  // sum of reserved notional of this client's live orders
    int open_orders = 0;             // this client's admitted-not-closed orders
  };

  // True when the best-effort per-day hub order-flow audit stream is enabled.
  bool FlowJournalOn() const { return risk_.order_flow_journal && !risk_.journal_dir.empty(); }

  void OnAck(const std::string& id, bool ok, const std::string& err);
  void OnFill(const std::string& id, const Fill& f);
  void OnCancel(const std::string& id, bool ok);
  int ClientFor(const std::string& id);  // -1 if unknown

  // Send the client a failing ack for a submit the gate refused; no forward.
  void RejectSubmit(int client, const std::string& id, const std::string& reason);
  void ReleaseOpen(Route& r);      // free a route's reserved open notional once; caller holds mu_
  long CurrentTradingDay() const;  // caller holds mu_
  long NowMonoMs() const;          // steady-clock ms, or the test override
  // Drop the dup-ring entry for a now-terminal order id, if present. Caller holds mu_.
  void RemoveDupEntry(const std::string& id);
  // Track/untrack an order's (symbol, side) for post-death fill journaling. Kept
  // past OnClientDisconnect on purpose; dropped only on a hub-observed terminal.
  // Caller holds mu_.
  void RememberOrder(const std::string& id, const std::string& symbol, Side side, long shares);
  void ForgetOrder(const std::string& id);

  OrderBackend* backend_;
  SendFn send_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, Route> routes_;
  // Ids of currently-open routes; bounds the per-submit self-match scan to live
  // orders instead of every route accumulated since the connection opened.
  std::unordered_set<std::string> open_ids_;
  std::unordered_map<int, ClientStats> clients_;
  long start_epoch_s_ = 0;
  std::int64_t account_open_notional_cents_ = 0;  // reserved notional of all live orders
  std::int64_t account_day_realized_cents_ = 0;   // filled value since the trading-day boundary
  long current_trading_day_ = 0;                  // YYYYMMDD the realized total belongs to
  long forced_trading_day_ = -1;                  // test override; <0 uses wall clock
  std::deque<DupEntry> dup_ring_;                 // admitted submits within the dup window
  long forced_mono_ms_ = -1;                      // test override; <0 uses steady clock
  // Surviving id->{symbol,side}; outlives routes_ so an orphan fill on a
  // disconnected client's id can still be named for the journal.
  std::unordered_map<std::string, OrderMeta> order_meta_;
  std::deque<std::string> meta_fifo_;  // insertion order, for the kOrderMetaCap bound
  // Per-symbol reference price for the collar: this account's own last fill.
  // The hub has no quote feed, so fills are the only price it observes.
  std::unordered_map<std::string, Cents> last_fill_price_cents_;
  RiskConfig risk_;
  RiskGate gate_{risk_};  // stateless money-guard over the state above; caller holds mu_
  std::atomic<bool> admin_halt_{false};
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ORDER_HUB_H_
