#include "failure_halt.h"

namespace kairos::exec {

void FailureHalt::RegisterFailure(const std::string& reason, int max_failures) {
  ++consecutive_failures_;
  if (max_failures > 0 && consecutive_failures_ >= max_failures) {
    halted_ = true;
    halt_reason_ = "halted: " + std::to_string(consecutive_failures_) +
                   " consecutive order failures (" + reason + ")";
  }
}

}  // namespace kairos::exec
