#ifndef KAIROS_EXEC_EVENT_SINK_H_
#define KAIROS_EXEC_EVENT_SINK_H_

#include "event.h"

namespace kairos::exec {

// The only notification seam the engine holds. Emit is non-blocking and must
// never throw (a notification failure must not affect trading).
class EventSink {
 public:
  virtual ~EventSink() = default;
  virtual void Emit(const Event& ev) = 0;
};

// Default when notifications are off: drop everything.
class NullEventSink : public EventSink {
 public:
  void Emit(const Event&) override {}
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_EVENT_SINK_H_
