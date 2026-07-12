#ifndef KAIROS_EXEC_FAILURE_HALT_H_
#define KAIROS_EXEC_FAILURE_HALT_H_

#include <string>

namespace kairos::exec {

// Fail-closed order-failure streak: counts consecutive submit-rejects / ack
// timeouts and latches a halt once the count reaches max_failures (a good ack
// clears the streak). Guarded by the engine's mu_; carries no mutex of its own.
class FailureHalt {
 public:
  // Count one order failure; latch the halt once the consecutive count reaches
  // max_failures (<= 0 disables the cap). reason names the failing path.
  void RegisterFailure(const std::string& reason, int max_failures);
  // A successful ack clears the fail-closed streak.
  void Reset() { consecutive_failures_ = 0; }
  bool halted() const { return halted_; }
  // Terminal crash reason; never empty once halted.
  const std::string& reason() const { return halt_reason_; }

 private:
  int consecutive_failures_ = 0;
  bool halted_ = false;
  std::string halt_reason_;
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_FAILURE_HALT_H_
