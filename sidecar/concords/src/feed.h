#pragma once

#include <string>

namespace kairos::concords {
int RunFeed(const std::string& config_path);

// Quotes are expected on TWSE ~08:30-13:30; a gap of >= threshold_s within that
// window means the concords ticker SDK froze and the feed must rebuild (0 = off).
inline bool FeedStale(int now_hhmm, long long secs_since_quote, int threshold_s) {
  if (threshold_s <= 0) return false;
  return now_hhmm >= 830 && now_hhmm < 1330 && secs_since_quote >= threshold_s;
}
}  // namespace kairos::concords
