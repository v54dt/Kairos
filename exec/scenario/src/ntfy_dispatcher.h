#ifndef KAIROS_EXEC_NTFY_DISPATCHER_H_
#define KAIROS_EXEC_NTFY_DISPATCHER_H_

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include "event.h"
#include "event_sink.h"
#include "notify_config.h"
#include "transport.h"

namespace kairos::exec {

// EventSink that routes each event to ntfy: per-category priority/tags, severity
// floor, per-key dedupe, and one global token bucket (ntfy.sh cap), then posts
// JSON via the Transport. All policy lives here; the engine just emits events.
class NtfyDispatcher : public EventSink {
 public:
  using Clock = std::function<std::chrono::steady_clock::time_point()>;
  NtfyDispatcher(NotifyConfig cfg, Transport* transport, Clock now = {});

  void Emit(const Event& ev) override;

 private:
  NotifyConfig cfg_;
  Transport* transport_;
  std::map<EventCategory, RouteConfig> routes_;
  Clock now_;
  std::mutex mu_;
  std::map<std::string, std::chrono::steady_clock::time_point> last_sent_;
  double tokens_;
  std::chrono::steady_clock::time_point last_refill_;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_NTFY_DISPATCHER_H_
