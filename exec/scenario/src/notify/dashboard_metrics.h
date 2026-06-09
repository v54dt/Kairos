#ifndef KAIROS_EXEC_DASHBOARD_METRICS_H_
#define KAIROS_EXEC_DASHBOARD_METRICS_H_

// Reports real-order round-trip latency to the LatencyDashboard write API
// (POST <api_url>/order-metrics). Live orders only — paper has no broker RTT.

#include <optional>
#include <string>
#include <utility>

#include "transport.h"

namespace kairos::exec {

struct DashboardConfig {
  bool enabled = false;
  std::string api_url;  // e.g. "https://metrics.example.com/api"
  std::string broker_name = "scenario-trader";
};

class DashboardMetrics {
 public:
  DashboardMetrics(DashboardConfig cfg, Transport* transport)
      : cfg_(std::move(cfg)), transport_(transport) {}

  bool enabled() const { return cfg_.enabled; }

  // outcome must be a dashboard CHECK value:
  //   success | ack_timeout | submit_error | cancel_timeout | cancel_error
  void ReportOrder(int iteration_id, const std::string& outcome, const std::string& error_message,
                   std::optional<double> total_ms, std::optional<double> sdk_local_ms,
                   std::optional<double> ack_rtt_ms);

 private:
  DashboardConfig cfg_;
  Transport* transport_;  // not owned
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_DASHBOARD_METRICS_H_
