// Self-test for the event model + EventSink seam. No broker, no network.

#include <cstdio>
#include <string>
#include <vector>

#include "event.h"
#include "event_sink.h"
#include "test_check.h"

using namespace kairos::exec;

namespace {
class RecordingEventSink : public EventSink {
 public:
  void Emit(const Event& ev) override { events.push_back(ev); }
  std::vector<Event> events;
};
}  // namespace

int main() {
  CHECK(std::string(CategoryName(EventCategory::kFill)) == "fill");
  CHECK(std::string(CategoryName(EventCategory::kQuoteStall)) == "quoteStall");

  RecordingEventSink sink;
  EventSink& as_iface = sink;
  as_iface.Emit(Event{EventCategory::kFill,
                      Severity::kInfo,
                      "2330",
                      "fill:1",
                      {{"shares", "1"}, {"price", "2360.00"}}});
  as_iface.Emit(Event{EventCategory::kError, Severity::kError, "2330", "reject:1", {}});

  CHECK(sink.events.size() == 2);
  CHECK(sink.events[0].category == EventCategory::kFill);
  CHECK(sink.events[0].severity == Severity::kInfo);
  CHECK(sink.events[0].symbol == "2330");
  CHECK(sink.events[0].fields.size() == 2);
  CHECK(sink.events[0].fields[1].first == "price");
  CHECK(sink.events[0].fields[1].second == "2360.00");
  CHECK(sink.events[1].category == EventCategory::kError);
  CHECK(sink.events[1].severity == Severity::kError);

  // NullEventSink drops without effect.
  NullEventSink null_sink;
  static_cast<EventSink&>(null_sink).Emit(sink.events[0]);

  if (g_failures == 0) {
    std::printf("test_event: OK\n");
    return 0;
  }
  std::printf("test_event: FAILED %d check(s)\n", g_failures);
  return 1;
}
