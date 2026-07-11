#include "dashboard_metrics.h"

#include <chrono>
#include <format>

#include "json_util.h"

namespace kairos::exec {

namespace {
std::string NowIso8601() {
  auto now_s = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now_s);
}
std::string OptMs(const std::optional<double>& v) {
  return v.has_value() ? std::format("{:.3f}", *v) : std::string("null");
}
}  // namespace

void DashboardMetrics::ReportOrder(int iteration_id, const std::string& outcome,
                                   const std::string& error_message, std::optional<double> total_ms,
                                   std::optional<double> sdk_local_ms,
                                   std::optional<double> ack_rtt_ms,
                                   std::optional<double> cancel_rtt_ms) {
  if (!cfg_.enabled || transport_ == nullptr || cfg_.api_url.empty()) return;

  std::string err_json = error_message.empty() ? "null" : JsonString(error_message);
  std::string body = std::format(
      R"({{"timestamp":"{}","iteration_id":{},"broker":{},"outcome":{},"error_message":{},)"
      R"("total_ms":{},"sdk_local_ms":{},"ack_rtt_ms":{},"cancel_rtt_ms":{}}})",
      NowIso8601(), iteration_id, JsonString(cfg_.broker_name), JsonString(outcome), err_json,
      OptMs(total_ms), OptMs(sdk_local_ms), OptMs(ack_rtt_ms), OptMs(cancel_rtt_ms));

  HttpRequest req;
  req.url = cfg_.api_url + "/order-metrics";
  req.body = std::move(body);
  req.headers.push_back("Content-Type: application/json");
  transport_->Post(std::move(req));
}

}  // namespace kairos::exec
