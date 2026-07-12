#ifndef KAIROS_EXEC_TIME_UTIL_H_
#define KAIROS_EXEC_TIME_UTIL_H_

// Shared clock helpers. System-clock time is wall-clock (journal timestamps,
// epoch seconds); steady-clock time is monotonic (dedup windows, latency spans)
// and must never be compared against wall-clock values.

#include <chrono>

namespace kairos::exec {

// Wall-clock microseconds since the Unix epoch.
inline long SystemNowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Monotonic milliseconds since the steady-clock epoch (comparable only to itself).
inline long long SteadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// A steady-clock duration as fractional milliseconds (latency reporting).
inline double SteadyMillis(std::chrono::steady_clock::duration d) {
  return std::chrono::duration_cast<std::chrono::microseconds>(d).count() / 1000.0;
}

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_TIME_UTIL_H_
