// Self-test for DashboardReporter: no-op when unconfigured, and the payload +
// cancel-RTT match/clear when configured. No network (fake Transport).

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "dashboard_metrics.h"
#include "dashboard_reporter.h"
#include "test_check.h"
#include "transport.h"

using namespace kairos::exec;

namespace {

class RecordingTransport : public Transport {
 public:
  void Post(HttpRequest req) override { reqs.push_back(std::move(req)); }
  std::vector<HttpRequest> reqs;
};

bool Contains(const std::string& s, const std::string& sub) {
  return s.find(sub) != std::string::npos;
}

using TP = std::chrono::steady_clock::time_point;
TP At(long ms) { return TP{} + std::chrono::milliseconds(ms); }

// Paper (live=false) reports nothing even with a dashboard attached.
void TestNoOpNotLive() {
  RecordingTransport tr;
  DashboardMetrics d({true, "https://x/api", "scenario-trader"}, &tr);
  DashboardReporter r;
  r.SetDashboard(&d, /*live=*/false);
  r.ReportAckSuccess(1, At(0), At(2), At(10));
  r.StampCancel("o1", 1, At(0));
  r.ReportCancel("o1", true, At(5));
  CHECK(tr.reqs.empty());
}

// No dashboard configured: every call is a safe no-op (no null deref).
void TestNoOpUnconfigured() {
  RecordingTransport tr;
  DashboardReporter r;
  r.SetDashboard(nullptr, true);
  r.ReportAckSuccess(1, At(0), At(2), At(10));
  r.ReportReject(2, "bad", At(0), At(2));
  r.ReportAckTimeout(3, 250, At(0), At(2), At(10));
  r.StampCancel("o1", 1, At(0));
  r.ReportCancel("o1", true, At(5));
  CHECK(tr.reqs.empty());
}

void TestAckSuccessPayload() {
  RecordingTransport tr;
  DashboardMetrics d({true, "https://x/api", "scenario-trader"}, &tr);
  DashboardReporter r;
  r.SetDashboard(&d, true);
  r.ReportAckSuccess(7, At(0), At(2), At(10));
  CHECK(tr.reqs.size() == 1);
  const std::string& b = tr.reqs[0].body;
  CHECK(Contains(b, "\"iteration_id\":7"));
  CHECK(Contains(b, "\"outcome\":\"success\""));
  CHECK(Contains(b, "\"total_ms\":10.000"));
  CHECK(Contains(b, "\"sdk_local_ms\":2.000"));
  CHECK(Contains(b, "\"ack_rtt_ms\":8.000"));
  CHECK(Contains(b, "\"cancel_rtt_ms\":null"));
}

void TestRejectPayload() {
  RecordingTransport tr;
  DashboardMetrics d({true, "https://x/api", "scenario-trader"}, &tr);
  DashboardReporter r;
  r.SetDashboard(&d, true);
  r.ReportReject(3, "rejected by broker", At(0), At(2));
  CHECK(tr.reqs.size() == 1);
  const std::string& b = tr.reqs[0].body;
  CHECK(Contains(b, "\"outcome\":\"submit_error\""));
  CHECK(Contains(b, "\"error_message\":\"rejected by broker\""));
  CHECK(Contains(b, "\"total_ms\":null"));
  CHECK(Contains(b, "\"sdk_local_ms\":2.000"));
  CHECK(Contains(b, "\"ack_rtt_ms\":null"));
}

void TestAckTimeoutPayload() {
  RecordingTransport tr;
  DashboardMetrics d({true, "https://x/api", "scenario-trader"}, &tr);
  DashboardReporter r;
  r.SetDashboard(&d, true);
  r.ReportAckTimeout(5, 250, At(0), At(2), At(30));
  CHECK(tr.reqs.size() == 1);
  const std::string& b = tr.reqs[0].body;
  CHECK(Contains(b, "\"outcome\":\"ack_timeout\""));
  CHECK(Contains(b, "\"error_message\":\"ack timeout after 250ms\""));
  CHECK(Contains(b, "\"total_ms\":30.000"));
  CHECK(Contains(b, "\"sdk_local_ms\":2.000"));
  CHECK(Contains(b, "\"ack_rtt_ms\":250.000"));
}

// The cancel RTT is stamped and reported against the matching pending id, then
// cleared; a non-matching id is ignored, and a second report is dropped.
void TestCancelMatchAndClear() {
  RecordingTransport tr;
  DashboardMetrics d({true, "https://x/api", "scenario-trader"}, &tr);
  DashboardReporter r;
  r.SetDashboard(&d, true);
  r.StampCancel("o1", 9, At(100));
  r.ReportCancel("other", true, At(150));  // non-matching id: ignored
  CHECK(tr.reqs.empty());
  r.ReportCancel("o1", true, At(140));  // matches -> reports, then clears
  CHECK(tr.reqs.size() == 1);
  const std::string& b = tr.reqs[0].body;
  CHECK(Contains(b, "\"iteration_id\":9"));
  CHECK(Contains(b, "\"outcome\":\"success\""));
  CHECK(Contains(b, "\"cancel_rtt_ms\":40.000"));
  r.ReportCancel("o1", true, At(200));  // already cleared: dropped
  CHECK(tr.reqs.size() == 1);
}

void TestCancelError() {
  RecordingTransport tr;
  DashboardMetrics d({true, "https://x/api", "scenario-trader"}, &tr);
  DashboardReporter r;
  r.SetDashboard(&d, true);
  r.StampCancel("o2", 4, At(0));
  r.ReportCancel("o2", false, At(12));
  CHECK(tr.reqs.size() == 1);
  CHECK(Contains(tr.reqs[0].body, "\"outcome\":\"cancel_error\""));
}

}  // namespace

int main() {
  TestNoOpNotLive();
  TestNoOpUnconfigured();
  TestAckSuccessPayload();
  TestRejectPayload();
  TestAckTimeoutPayload();
  TestCancelMatchAndClear();
  TestCancelError();
  if (g_failures == 0) {
    std::printf("test_dashboard_reporter: OK\n");
    return 0;
  }
  std::printf("test_dashboard_reporter: FAILED %d check(s)\n", g_failures);
  return 1;
}
