#ifndef KAIROS_EXEC_NOTIFY_CONFIG_H_
#define KAIROS_EXEC_NOTIFY_CONFIG_H_

#include <map>
#include <string>
#include <vector>

#include "event.h"

namespace kairos::exec {

// Per-category routing + policy (built-in defaults; see DefaultRoutes).
struct RouteConfig {
  bool enabled = true;
  int priority = 3;  // ntfy 1..5
  std::vector<std::string> tags;
  long dedup_window_s = 0;         // 0 = no dedupe
  bool bypass_rate_limit = false;  // one-shot terminal events always send
};

struct NotifyConfig {
  bool enabled = false;
  std::string base_url;  // self-hosted ntfy base URL, e.g. https://ntfy.example.com
  std::string topic;
  std::string token;  // ntfy access token (Bearer)
  Severity min_severity = Severity::kInfo;
  // One token bucket for the ntfy.sh upstream cap (~250 msgs / 12h per IP).
  long rate_capacity = 250;
  double rate_refill_per_h = 20.0;
};

std::map<EventCategory, RouteConfig> DefaultRoutes();

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_NOTIFY_CONFIG_H_
