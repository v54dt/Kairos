#ifndef KAIROS_EXEC_ENGINE_H_
#define KAIROS_EXEC_ENGINE_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "dashboard_metrics.h"
#include "engine_logic.h"
#include "event_sink.h"
#include "order_backend.h"
#include "order_journal.h"
#include "quote_book.h"
#include "quote_source.h"
#include "scenario.h"

namespace kairos::exec {

// The engine's two time sources, injected so a replay can drive them coherently.
// wall: local/session gating (date strings + HHMM window). mono: latency/pacing.
// Both default to the real system/steady clocks.
struct EngineClock {
  std::function<std::chrono::system_clock::time_point()> wall = [] {
    return std::chrono::system_clock::now();
  };
  std::function<std::chrono::steady_clock::time_point()> mono = [] {
    return std::chrono::steady_clock::now();
  };
};

// Drives a scenario: consumes the core quote feed into a QuoteBook, and on each
// tick decides place / re-peg / nothing (engine_logic), routing orders through
// the OrderBackend behind a 1 req/s gate. Run() blocks until the budget is met,
// the window closes, or RequestStop().
class ScenarioEngine {
 public:
  ScenarioEngine(Scenario scenario, OrderBackend* backend, EventSink* sink, QuoteSource* quotes,
                 EngineClock clock = {});

  // 0 = normal (complete/shutdown/incomplete). Non-zero = fail-closed exit the
  // supervisor classifies as crashed (halt on consecutive failures, or a live run
  // refused for lack of a journal).
  int Run();
  void RequestStop();
  void set_ignore_window(bool v) { ignore_window_ = v; }
  void set_dashboard(DashboardMetrics* d) { dashboard_ = d; }

 private:
  void OnAck(const std::string& id, bool ok, const std::string& err);
  void OnFill(const std::string& id, const Fill& f);
  void OnCancel(const std::string& id, bool ok);
  void OnDisconnect();
  void ClearResting();  // call under mu_
  // Move the still-unfilled part of the current resting order into the
  // possibly-live ledger (below) before we stop tracking it, so the sell cap
  // keeps counting it. Call under mu_.
  void AbandonResting();
  // Count one order failure (submit-reject or ack-timeout); halt the run once the
  // consecutive count reaches the configured cap. Call under mu_.
  void RegisterFailure(const std::string& reason);
  // Stamp the RTT-start for a cancel just issued so OnCancel can report its
  // round-trip to the dashboard. Call outside mu_ (locks internally).
  void StampCancel(const std::string& id, int seq);
  void SdkGate();
  std::string NextOrderId();

  Scenario s_;
  OrderBackend* backend_;
  EventSink* sink_;
  DashboardMetrics* dashboard_ = nullptr;
  QuoteBook book_;
  QuoteSource* quotes_;
  EngineClock clock_;
  OrderJournal journal_;

  std::mutex mu_;
  std::condition_variable cv_;
  Accounting acct_;
  RestingOrder resting_;
  std::string resting_id_;
  long resting_filled_ = 0;
  // Orders dropped from resting_ (ack-timeout, or a re-peg cancel the broker
  // rejected) that may still be live at the broker: id -> shares not yet filled.
  // Counted against the sell position cap until a fill or a confirmed cancel
  // resolves them, so filled + in-flight can never oversell the position.
  std::unordered_map<std::string, long> inflight_lost_;
  bool resting_acked_ = false;  // broker confirmed the working order (OnSubmit)
  bool cancelling_ = false;     // cancel issued for re-peg, awaiting OnCancel/full fill
  int resting_seq_ = 0;         // order id's sequence number (dashboard iteration_id)
  std::chrono::steady_clock::time_point resting_t_start_;   // before Submit (post-gate)
  std::chrono::steady_clock::time_point resting_t_submit_;  // after Submit returns
  std::chrono::steady_clock::time_point cancel_t_sent_;     // RTT start of the pending cancel
  std::string cancel_pending_id_;  // order whose cancel RTT is outstanding (one at a time)
  int cancel_seq_ = 0;             // that order's dashboard iteration_id
  bool complete_ = false;
  int consecutive_failures_ = 0;  // reset by a successful ack; halts the run at the cap
  bool halted_ = false;           // fail-closed: stop placing orders and exit non-zero
  std::string halt_reason_;       // terminal event / crash reason (never empty when halted_)
  bool journal_ok_ = false;       // a usable run-state journal is open
  bool quote_stalled_ = false;    // quote-stall alert armed/fired (main-thread only)
  std::atomic<bool> stop_{false};
  bool ignore_window_ = false;
  int schedule_start_min_ =
      -1;  // first in-window minute => twap spreads from here, not window_start

  std::mutex sdk_mu_;
  std::chrono::steady_clock::time_point last_sdk_{};
  long order_seq_ = 0;
  std::string oid_prefix_;
  int last_milestone_pct_ = 0;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ENGINE_H_
