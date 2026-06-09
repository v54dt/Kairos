#include "ntfy_dispatcher.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "json_util.h"

namespace kairos::exec {

std::map<EventCategory, RouteConfig> DefaultRoutes() {
  return {
      {EventCategory::kStart, {true, 3, {}, 0}},
      {EventCategory::kSubmit, {false, 2, {}, 0}},  // off by default (noisy)
      {EventCategory::kFill, {true, 3, {}, 0}},
      {EventCategory::kPartialFill, {true, 2, {}, 0}},
      {EventCategory::kMilestone, {true, 4, {}, 0}},
      {EventCategory::kComplete, {true, 4, {}, 0}},
      {EventCategory::kShutdown, {true, 4, {}, 0}},
      {EventCategory::kError, {true, 5, {}, 300}},
      {EventCategory::kDisconnect, {true, 5, {}, 300}},
      {EventCategory::kReconnect, {true, 3, {}, 0}},
      {EventCategory::kQuoteStall, {false, 2, {}, 600}},  // off by default
  };
}

namespace {

std::string RenderTitle(const Event& ev) {
  std::string t = ev.symbol.empty() ? "" : ev.symbol + " ";
  std::string name = CategoryName(ev.category);
  if (!name.empty()) name[0] = static_cast<char>(std::toupper(name[0]));
  t += name;
  return t;
}

std::string RenderMessage(const Event& ev) {
  std::string m;
  for (const auto& [k, v] : ev.fields) {
    if (!m.empty()) m += "\n";
    m += k + ": " + v;
  }
  return m;
}

std::string TagsJson(const std::vector<std::string>& tags) {
  std::string s = "[";
  for (std::size_t i = 0; i < tags.size(); ++i) {
    if (i) s += ",";
    s += JsonString(tags[i]);
  }
  s += "]";
  return s;
}

}  // namespace

NtfyDispatcher::NtfyDispatcher(NotifyConfig cfg, Transport* transport, Clock now)
    : cfg_(std::move(cfg)),
      transport_(transport),
      routes_(DefaultRoutes()),
      now_(now ? std::move(now) : [] { return std::chrono::steady_clock::now(); }),
      tokens_(static_cast<double>(cfg_.rate_capacity)),
      last_refill_(now_()) {}

void NtfyDispatcher::Emit(const Event& ev) {
  if (!cfg_.enabled) return;
  auto it = routes_.find(ev.category);
  RouteConfig route = it != routes_.end() ? it->second : RouteConfig{};
  if (!route.enabled) return;
  if (static_cast<int>(ev.severity) < static_cast<int>(cfg_.min_severity)) return;

  {
    std::lock_guard<std::mutex> lock(mu_);
    auto now = now_();
    if (!ev.dedup_key.empty() && route.dedup_window_s > 0) {
      auto f = last_sent_.find(ev.dedup_key);
      if (f != last_sent_.end() && now - f->second < std::chrono::seconds(route.dedup_window_s)) {
        return;
      }
      last_sent_[ev.dedup_key] = now;
    }
    double hours = std::chrono::duration<double>(now - last_refill_).count() / 3600.0;
    tokens_ =
        std::min(static_cast<double>(cfg_.rate_capacity), tokens_ + hours * cfg_.rate_refill_per_h);
    last_refill_ = now;
    if (tokens_ < 1.0) return;  // upstream rate cap — drop
    tokens_ -= 1.0;
  }

  HttpRequest req;
  req.url = cfg_.base_url;
  req.body = "{\"topic\":" + JsonString(cfg_.topic) + ",\"title\":" + JsonString(RenderTitle(ev)) +
             ",\"message\":" + JsonString(RenderMessage(ev)) +
             ",\"priority\":" + std::to_string(route.priority) +
             ",\"tags\":" + TagsJson(route.tags) + "}";
  req.headers.push_back("Content-Type: application/json");
  if (!cfg_.token.empty()) req.headers.push_back("Authorization: Bearer " + cfg_.token);
  transport_->Post(std::move(req));
}

}  // namespace kairos::exec
