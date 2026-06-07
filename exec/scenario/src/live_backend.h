#ifndef KAIROS_EXEC_LIVE_BACKEND_H_
#define KAIROS_EXEC_LIVE_BACKEND_H_

#include <memory>

#include "order_backend.h"
#include "scenario.h"

namespace kairos::exec {

// A live broker order backend for the scenario, or nullptr if this build has no
// broker SDK (then --live is unavailable). Defined by concords_order_backend.cpp
// when the SDK is present, else by live_backend_stub.cpp.
std::unique_ptr<OrderBackend> MakeLiveBackend(const Scenario& s);

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_LIVE_BACKEND_H_
