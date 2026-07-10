#ifndef KAIROS_EXEC_SIM_FAULT_CONFIG_H_
#define KAIROS_EXEC_SIM_FAULT_CONFIG_H_

#include <cstdint>

namespace kairos::exec {

// Sim-only fault-injection knobs for kairos_sim_hubd. Every field defaults to the
// no-fault value, so a default-constructed FaultConfig makes the sim hub behave
// exactly as it does today (byte-compatible passthrough). All randomness is drawn
// from one seeded LCG (seed), so a given config + seed replays an identical fault
// sequence, which the regression drills depend on.
struct FaultConfig {
  std::uint64_t seed = 0;        // LCG seed for every probabilistic draw
  long ack_delay_ms = 0;         // fixed delay added to every ok submit ack
  long ack_jitter_ms = 0;        // extra uniform-random [0, jitter] ms on top of the delay
  double ack_drop_rate = 0.0;    // probability an ok submit ack is never sent
  double reject_rate = 0.0;      // probability a submit is rejected with an error
  long partial_fill = 0;         // split each fill into this many partials (<=1 = whole)
  long disconnect_after_n = 0;   // drop the client link after this many submits (0 = never)
  long disconnect_every_ms = 0;  // drop the client link every this many ms (0 = never)

  bool Enabled() const {
    return ack_delay_ms > 0 || ack_jitter_ms > 0 || ack_drop_rate > 0.0 || reject_rate > 0.0 ||
           partial_fill > 1 || disconnect_after_n > 0 || disconnect_every_ms > 0;
  }
};

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SIM_FAULT_CONFIG_H_
