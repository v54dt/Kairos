#include "live_backend.h"

namespace kairos::exec {

// Built when the concords stock SDK is absent (e.g. CI): --live is unavailable.
std::unique_ptr<OrderBackend> MakeLiveBackend(const UserCreds&) { return nullptr; }

}  // namespace kairos::exec
