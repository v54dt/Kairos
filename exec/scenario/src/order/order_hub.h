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

  // Account risk-gate limits. All numeric caps are 0 = disabled; notional caps
  // are in Cents. self_match_protection defaults on so it is fail-closed by default.
  struct RiskConfig {
    long max_account_notional_cents = 0;
    int max_open_orders_per_client = 0;
    long max_open_notional_per_client_cents = 0;
    bool self_match_protection = true;
    long max_order_shares = 0;     // per-submit share hard cap; 0 = disabled
    long dup_order_window_ms = 0;  // reject an identical resubmit within this window; 0 = disabled
    long price_collar_pct = 0;     // reject a price this far from the last fill; 0 = disabled
    std::string halt_file_path;
  };

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

 private:
  // Lifecycle of one live order, derived from the routing the hub already does.
  struct Route {
    int client = -1;
    long shares_remaining = 0;
    bool acked = false;
    bool closed = false;  // fully filled or successfully cancelled
    std::string symbol;
    Side side = Side::kBuy;
    Cents price = 0;  // reserved open notional == price * shares_remaining
  };

  // One admitted submit, kept in a bounded ring so a runaway strategy that
  // resubmits the same (symbol, side, shares, price) within the window is caught.
  struct DupEntry {
    std::string symbol;
    Side side = Side::kBuy;
    long shares = 0;
    Cents price = 0;
    long mono_ms = 0;
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

  void OnAck(const std::string& id, bool ok, const std::string& err);
  void OnFill(const std::string& id, const Fill& f);
  void OnCancel(const std::string& id, bool ok);
  int ClientFor(const std::string& id);  // -1 if unknown

  // Send the client a failing ack for a submit the gate refused; no forward.
  void RejectSubmit(int client, const std::string& id, const std::string& reason);
  void ReleaseOpen(Route& r);      // free a route's reserved open notional once; caller holds mu_
  long CurrentTradingDay() const;  // caller holds mu_
  long NowMonoMs() const;          // steady-clock ms, or the test override
  // Prune the dup ring to the window and report whether an identical admitted
  // submit is still within it. Caller holds mu_ and has checked the window > 0.
  bool DuplicateSubmit(const OrderSubmitMsg& o, long now_ms);
  // The account's last fill price for `symbol` if one exists (the collar
  // reference); false at cold start (no fill yet). Caller holds mu_.
  bool CollarReference(const std::string& symbol, Cents* ref) const;
  // True if the submit would cross this account's own open opposite-side order on
  // the same symbol (證交法 155-1-5); *other_id is the crossed order. Caller holds mu_.
  bool SelfMatchCross(const OrderSubmitMsg& o, std::string* other_id) const;

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
  // Per-symbol reference price for the collar: this account's own last fill.
  // The hub has no quote feed, so fills are the only price it observes.
  std::unordered_map<std::string, Cents> last_fill_price_cents_;
  RiskConfig risk_;
  std::atomic<bool> admin_halt_{false};
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ORDER_HUB_H_
