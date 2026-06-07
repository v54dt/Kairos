#include "live_backend.h"

namespace kairos::exec {

// Built when the concords stock SDK is absent (e.g. CI): --live is unavailable.
std::unique_ptr<OrderBackend> MakeLiveBackend(const Scenario&) { return nullptr; }

}  // namespace kairos::exec
