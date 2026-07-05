#ifndef KAIROS_EXEC_ORDER_HUB_H_
#define KAIROS_EXEC_ORDER_HUB_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
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

  // Force the trading day the day-notional reset compares against (tests only).
  void SetTradingDayForTest(long day);

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

  OrderBackend* backend_;
  SendFn send_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, Route> routes_;
  std::unordered_map<int, ClientStats> clients_;
  long start_epoch_s_ = 0;
  std::int64_t account_open_notional_cents_ = 0;  // reserved notional of all live orders
  std::int64_t account_day_realized_cents_ = 0;   // filled value since the trading-day boundary
  long current_trading_day_ = 0;                  // YYYYMMDD the realized total belongs to
  long forced_trading_day_ = -1;                  // test override; <0 uses wall clock
  RiskConfig risk_;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ORDER_HUB_H_
