#ifndef KAIROS_EXEC_DASHBOARD_REPORTER_H_
#define KAIROS_EXEC_DASHBOARD_REPORTER_H_

#include <chrono>
#include <string>

#include "dashboard_metrics.h"

namespace kairos::exec {

// Thin wrapper over DashboardMetrics for the engine's per-order latency reports.
// Every method is a no-op unless a dashboard is configured for a live run, so the
// engine can call it unconditionally. Owns the outstanding cancel-RTT bookkeeping
// (one cancel at a time); the engine passes the steady-clock timestamps in so the
// clock stays engine-owned. Cancel state is guarded by the engine's mu_.
class DashboardReporter {
 public:
  using TimePoint = std::chrono::steady_clock::time_point;

  void SetDashboard(DashboardMetrics* dashboard, bool live) {
    dashboard_ = dashboard;
    live_ = live;
  }

  // A confirmed working order: total / sdk-local / ack round-trip latencies.
  void ReportAckSuccess(int seq, TimePoint t_start, TimePoint t_submit, TimePoint now);
  // A submit reject: only the sdk-local leg is known.
  void ReportReject(int seq, const std::string& err, TimePoint t_start, TimePoint t_submit);
  // An ack-timeout local reject: the reason carries the elapsed budget.
  void ReportAckTimeout(int seq, long since_submit, TimePoint t_start, TimePoint t_submit,
                        TimePoint now);
  // Stamp the RTT-start for a cancel just issued (one outstanding at a time).
  void StampCancel(const std::string& id, int seq, TimePoint now);
  // Report a cancel's round-trip against the stamped pending cancel, then clear it.
  // Ignores cancels for a non-matching id.
  void ReportCancel(const std::string& id, bool ok, TimePoint now);

 private:
  bool Active() const { return dashboard_ != nullptr && live_; }

  DashboardMetrics* dashboard_ = nullptr;  // not owned
  bool live_ = false;
  TimePoint cancel_t_sent_;        // RTT start of the pending cancel
  std::string cancel_pending_id_;  // order whose cancel RTT is outstanding
  int cancel_seq_ = 0;             // that order's dashboard iteration_id
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_DASHBOARD_REPORTER_H_
