// Self-test for DashboardMetrics payload + gating. No network (fake Transport).

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "dashboard_metrics.h"
#include "transport.h"

using namespace kairos::exec;

static int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

namespace {
class RecordingTransport : public Transport {
 public:
  void Post(HttpRequest req) override { reqs.push_back(std::move(req)); }
  std::vector<HttpRequest> reqs;
};
bool Contains(const std::string& s, const std::string& sub) {
  return s.find(sub) != std::string::npos;
}
}  // namespace

int main() {
  // disabled -> nothing
  {
    RecordingTransport tr;
    DashboardMetrics d({false, "https://x/api", "scenario-trader"}, &tr);
    d.ReportOrder(1, "success", "", 12.5, 1.0, 11.5);
    CHECK(tr.reqs.empty());
  }

  // enabled -> POST to /order-metrics with the expected fields (no metric_type)
  {
    RecordingTransport tr;
    DashboardMetrics d({true, "https://x/api", "scenario-trader"}, &tr);
    d.ReportOrder(7, "success", "", 12.5, 1.25, 11.25);
    CHECK(tr.reqs.size() == 1);
    CHECK(tr.reqs[0].url == "https://x/api/order-metrics");
    const std::string& b = tr.reqs[0].body;
    CHECK(Contains(b, "\"iteration_id\":7"));
    CHECK(Contains(b, "\"broker\":\"scenario-trader\""));
    CHECK(Contains(b, "\"outcome\":\"success\""));
    CHECK(Contains(b, "\"total_ms\":12.500"));
    CHECK(Contains(b, "\"sdk_local_ms\":1.250"));
    CHECK(Contains(b, "\"ack_rtt_ms\":11.250"));
    CHECK(Contains(b, "\"error_message\":null"));
    CHECK(Contains(b, "\"cancel_rtt_ms\":null"));
    CHECK(!Contains(b, "metric_type"));  // dropped (server ignores it)
  }

  // reject with error + null timings
  {
    RecordingTransport tr;
    DashboardMetrics d({true, "https://x/api", "scenario-trader"}, &tr);
    d.ReportOrder(3, "submit_error", "rejected by broker", std::nullopt, 0.9, std::nullopt);
    CHECK(tr.reqs.size() == 1);
    const std::string& b = tr.reqs[0].body;
    CHECK(Contains(b, "\"outcome\":\"submit_error\""));
    CHECK(Contains(b, "\"error_message\":\"rejected by broker\""));
    CHECK(Contains(b, "\"total_ms\":null"));
    CHECK(Contains(b, "\"sdk_local_ms\":0.900"));
  }

  // ack timeout -> own outcome, elapsed-at-timeout in ack_rtt_ms, cancel null
  {
    RecordingTransport tr;
    DashboardMetrics d({true, "https://x/api", "scenario-trader"}, &tr);
    d.ReportOrder(5, "ack_timeout", "ack timeout after 800ms", 812.0, 3.0, 800.0);
    CHECK(tr.reqs.size() == 1);
    const std::string& b = tr.reqs[0].body;
    CHECK(Contains(b, "\"outcome\":\"ack_timeout\""));
    CHECK(Contains(b, "\"error_message\":\"ack timeout after 800ms\""));
    CHECK(Contains(b, "\"total_ms\":812.000"));
    CHECK(Contains(b, "\"ack_rtt_ms\":800.000"));
    CHECK(Contains(b, "\"cancel_rtt_ms\":null"));
  }

  // cancel RTT -> only cancel_rtt_ms populated, order timings null
  {
    RecordingTransport tr;
    DashboardMetrics d({true, "https://x/api", "scenario-trader"}, &tr);
    d.ReportOrder(9, "success", "", std::nullopt, std::nullopt, std::nullopt, 4.250);
    CHECK(tr.reqs.size() == 1);
    const std::string& b = tr.reqs[0].body;
    CHECK(Contains(b, "\"outcome\":\"success\""));
    CHECK(Contains(b, "\"total_ms\":null"));
    CHECK(Contains(b, "\"ack_rtt_ms\":null"));
    CHECK(Contains(b, "\"cancel_rtt_ms\":4.250"));
  }

  // cancel rejected -> cancel_error with the RTT still reported
  {
    RecordingTransport tr;
    DashboardMetrics d({true, "https://x/api", "scenario-trader"}, &tr);
    d.ReportOrder(11, "cancel_error", "", std::nullopt, std::nullopt, std::nullopt, 6.000);
    CHECK(tr.reqs.size() == 1);
    const std::string& b = tr.reqs[0].body;
    CHECK(Contains(b, "\"outcome\":\"cancel_error\""));
    CHECK(Contains(b, "\"cancel_rtt_ms\":6.000"));
  }

  if (g_failures == 0) {
    std::printf("test_dashboard_metrics: OK\n");
    return 0;
  }
  std::printf("test_dashboard_metrics: FAILED %d check(s)\n", g_failures);
  return 1;
}
