#ifndef KAIROS_EXEC_EVENT_SINK_H_
#define KAIROS_EXEC_EVENT_SINK_H_

#include <cstdio>
#include <vector>

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

// Prints one line per event to stdout so the state machine's alerts appear in the
// operator log (and are assertable end-to-end); never posts anywhere.
class LogEventSink : public EventSink {
 public:
  void Emit(const Event& ev) override {
    std::printf("event %s cat=%s sev=%d", ev.dedup_key.c_str(), CategoryName(ev.category),
                static_cast<int>(ev.severity));
    for (const auto& [k, v] : ev.fields) std::printf(" %s=%s", k.c_str(), v.c_str());
    std::printf("\n");
    std::fflush(stdout);
  }
};

// Fans one event out to several sinks (e.g. the operator log plus ntfy).
class TeeEventSink : public EventSink {
 public:
  explicit TeeEventSink(std::vector<EventSink*> sinks) : sinks_(std::move(sinks)) {}
  void Emit(const Event& ev) override {
    for (EventSink* s : sinks_) s->Emit(ev);
  }

 private:
  std::vector<EventSink*> sinks_;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_EVENT_SINK_H_
