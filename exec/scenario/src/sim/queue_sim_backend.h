#ifndef KAIROS_EXEC_SIM_QUEUE_SIM_BACKEND_H_
#define KAIROS_EXEC_SIM_QUEUE_SIM_BACKEND_H_

#include "sim_order_backend.h"

namespace kairos::exec {

// In-process realistic paper backend (default PAPER mode). Fed the same
// TopOfBook/Trade stream the strategy sees, it fills through the queue model
// instead of PaperOrderBackend's instant full fill, so a passive/join strategy's
// true fill rate is visible in paper. Distinct from SimOrderBackend only in name
// and intent; it is also the extension point for shadow paper (running one
// alongside a live backend and reporting fill-rate divergence).
class QueueSimBackend : public SimOrderBackend {
 public:
  using SimOrderBackend::SimOrderBackend;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SIM_QUEUE_SIM_BACKEND_H_
