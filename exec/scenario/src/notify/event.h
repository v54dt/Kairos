#ifndef KAIROS_EXEC_EVENT_H_
#define KAIROS_EXEC_EVENT_H_

#include <string>
#include <utility>
#include <vector>

namespace kairos::exec {

enum class EventCategory {
  kStart,
  kSubmit,
  kFill,
  kPartialFill,
  kMilestone,
  kComplete,
  kShutdown,
  kIncomplete,
  kError,
  kDisconnect,
  kReconnect,
  kQuoteStall,
};

enum class Severity { kInfo, kWarning, kError };

// Presentation-free event the engine emits. The dispatcher renders title/message
// /priority/tags from category + fields; the engine never speaks ntfy.
struct Event {
  EventCategory category;
  Severity severity = Severity::kInfo;
  std::string symbol;
  std::string dedup_key;  // engine-chosen identity for dedupe (may be empty)
  std::vector<std::pair<std::string, std::string>> fields;
};

inline const char* CategoryName(EventCategory c) {
  switch (c) {
    case EventCategory::kStart:
      return "start";
    case EventCategory::kSubmit:
      return "submit";
    case EventCategory::kFill:
      return "fill";
    case EventCategory::kPartialFill:
      return "partialFill";
    case EventCategory::kMilestone:
      return "milestone";
    case EventCategory::kComplete:
      return "complete";
    case EventCategory::kShutdown:
      return "shutdown";
    case EventCategory::kIncomplete:
      return "incomplete";
    case EventCategory::kError:
      return "error";
    case EventCategory::kDisconnect:
      return "disconnect";
    case EventCategory::kReconnect:
      return "reconnect";
    case EventCategory::kQuoteStall:
      return "quoteStall";
  }
  return "?";
}

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_EVENT_H_
