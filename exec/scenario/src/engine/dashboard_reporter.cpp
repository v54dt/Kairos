#include "dashboard_reporter.h"

#include <optional>

#include "time_util.h"  // SteadyMillis

namespace kairos::exec {

void DashboardReporter::ReportAckSuccess(int seq, TimePoint t_start, TimePoint t_submit,
                                         TimePoint now) {
  if (!Active()) return;
  dashboard_->ReportOrder(seq, "success", "", SteadyMillis(now - t_start),
                          SteadyMillis(t_submit - t_start), SteadyMillis(now - t_submit));
}

void DashboardReporter::ReportReject(int seq, const std::string& err, TimePoint t_start,
                                     TimePoint t_submit) {
  if (!Active()) return;
  dashboard_->ReportOrder(seq, "submit_error", err, std::nullopt, SteadyMillis(t_submit - t_start),
                          std::nullopt);
}

void DashboardReporter::ReportAckTimeout(int seq, long since_submit, TimePoint t_start,
                                         TimePoint t_submit, TimePoint now) {
  if (!Active()) return;
  dashboard_->ReportOrder(seq, "ack_timeout",
                          "ack timeout after " + std::to_string(since_submit) + "ms",
                          SteadyMillis(now - t_start), SteadyMillis(t_submit - t_start),
                          static_cast<double>(since_submit));
}

void DashboardReporter::StampCancel(const std::string& id, int seq, TimePoint now) {
  cancel_t_sent_ = now;
  cancel_pending_id_ = id;
  cancel_seq_ = seq;
}

void DashboardReporter::ReportCancel(const std::string& id, bool ok, TimePoint now) {
  if (!Active() || id != cancel_pending_id_) return;
  dashboard_->ReportOrder(cancel_seq_, ok ? "success" : "cancel_error", "", std::nullopt,
                          std::nullopt, std::nullopt, SteadyMillis(now - cancel_t_sent_));
  cancel_pending_id_.clear();
}

}  // namespace kairos::exec
