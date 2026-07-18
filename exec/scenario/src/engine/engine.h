#ifndef KAIROS_EXEC_ENGINE_H_
#define KAIROS_EXEC_ENGINE_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>

#include "dashboard_metrics.h"
#include "dashboard_reporter.h"
#include "engine_logic.h"
#include "event_sink.h"
#include "failure_halt.h"
#include "order_backend.h"
#include "order_journal.h"
#include "quote_book.h"
#include "quote_source.h"
#include "scenario.h"
#include "sell_cap_ledger.h"

namespace kairos::exec {

// Snapshot of one leg's fill accounting for the RoundTripRunner (PR4) to chain
// legs. avg_price_cents is 0 when there are no fills (no divide-by-zero).
struct LegResult {
  long filled_shares = 0;
  long filled_notional_cents = 0;
  long avg_price_cents = 0;
  bool complete = false;
  bool halted = false;
};

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
  // Thread-safe snapshot of the current fill accounting + terminal state.
  LegResult Result() const;
  void set_ignore_window(bool v) { ignore_window_ = v; }
  void set_dashboard(DashboardMetrics* d) { dash_.SetDashboard(d, s_.live); }

 private:
  void OnAck(const std::string& id, bool ok, const std::string& err);
  void OnFill(const std::string& id, const Fill& f);
  void OnCancel(const std::string& id, bool ok);
  // Hub confirmed the current resting order reached the broker: rebase the
  // ack-timeout clock so hub queue delay is not misread as broker silence. Only
  // the current, un-acked resting order is affected, and only once (fail-closed).
  void OnForwarded(const std::string& id);
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

  // Run() loop steps, each a verbatim block move from the original inline loop.
  // The mu_ precondition per step is load-bearing.
  enum class PaceDecision { kBreak, kContinue, kProceed };
  // Window/TWAP pacing for one tick. Call outside mu_ (locks internally only for
  // the wait-for-open cv wait); sets window_progress and may signal break/continue.
  PaceDecision PaceWindow(double& window_progress);
  // Quote staleness check + quote-stall alert. Call outside mu_.
  bool CheckStaleAndStall(const TopOfBook& tob, long age);
  // Ack-timeout watchdog: reason event -> dashboard -> AbandonResting ->
  // ClearResting -> RegisterFailure. Call under mu_; outputs the possibly-live id
  // to cancel outside the lock.
  void RunAckWatchdog(std::string& timed_out_id, int& timed_out_seq);
  // Place / re-peg dispatch. Call outside mu_ (re-takes mu_ at the mutation
  // sites). Returns true if the loop should break (done with no resting order).
  bool DispatchAction(const TopOfBook& tob, const RestingOrder& resting, long remaining,
                      double window_progress, long sell_cap_remaining, const std::string& rid,
                      int rid_seq, bool acked, bool cancelling);
  // Cancel the acked working order (if any) and drain the feed/backend. Call
  // outside mu_.
  void WindDown();
  // Terminal classification + stdout summary + terminal event. Takes mu_ once and
  // holds through the whole body. Returns the process exit code.
  int EmitTerminal();

  Scenario s_;
  OrderBackend* backend_;
  EventSink* sink_;
  DashboardReporter dash_;
  QuoteBook book_;
  QuoteSource* quotes_;
  EngineClock clock_;
  OrderJournal journal_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  Accounting acct_;
  RestingOrder resting_;
  std::string resting_id_;
  long resting_filled_ = 0;
  // Orders dropped from resting_ (ack-timeout, or a re-peg cancel the broker
  // rejected) that may still be live at the broker; counted against the sell
  // position cap so filled + in-flight can never oversell. Guarded by mu_.
  SellCapLedger sell_cap_;
  bool resting_acked_ = false;           // broker confirmed the working order (OnSubmit)
  bool resting_forwarded_used_ = false;  // hub-forwarded clock restart consumed (once per order)
  bool cancelling_ = false;              // cancel issued for re-peg, awaiting OnCancel/full fill
  int resting_seq_ = 0;                  // order id's sequence number (dashboard iteration_id)
  std::chrono::steady_clock::time_point resting_t_start_;   // before Submit (post-gate)
  std::chrono::steady_clock::time_point resting_t_submit_;  // after Submit returns
  bool complete_ = false;
  FailureHalt failure_halt_;    // fail-closed order-failure streak; guarded by mu_
  bool journal_ok_ = false;     // a usable run-state journal is open
  bool quote_stalled_ = false;  // quote-stall alert armed/fired (main-thread only)
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
