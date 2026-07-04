#ifndef KAIROS_EXEC_SIM_SESSION_SCHEDULE_H_
#define KAIROS_EXEC_SIM_SESSION_SCHEDULE_H_

#include <cstdint>

namespace kairos::exec {

// TWSE regular-session schedule for the offline auction sim, expressed as hhmm
// (hour*100 + minute) in Taipei local time. These mirror the engine's existing
// session-gating conventions (engine.cpp kMarketCloseHhmm = 1330) but live here
// so the sim never reaches into the engine.
constexpr int kOpenHhmm = 900;               // opening call-auction match
constexpr int kCloseWindowStartHhmm = 1325;  // closing call-auction accumulation opens
constexpr int kCloseHhmm = 1330;             // scheduled closing call-auction match

// Intraday/closing price stabilization: if the indicative auction price deviates
// beyond 3.5% of the reference, the closing match is delayed (延緩收盤) once.
constexpr double kStabilizationPct = 3.5;
constexpr long kStabilizationNum = 35;  // 3.5% == 35/1000, integer band test
constexpr long kStabilizationDen = 1000;
constexpr int kDelaySeconds = 180;  // 延緩收盤 extension: 3 minutes, applied at most once

// Pure UTC+8 conversion from a CLOCK_REALTIME microsecond timestamp (matching the
// wire's quoteTsUs / tradeTsUs) to Taipei local hhmm. No wall clock is read — all
// session decisions in the sim derive from event timestamps only.
inline int HhmmFromUs(std::int64_t ts_us) {
  std::int64_t secs = ts_us / 1000000;
  if (ts_us < 0 && ts_us % 1000000 != 0) --secs;  // floor toward -inf
  std::int64_t local = secs + 8 * 3600;
  std::int64_t tod = ((local % 86400) + 86400) % 86400;  // seconds since local midnight
  int hh = static_cast<int>(tod / 3600);
  int mm = static_cast<int>((tod % 3600) / 60);
  return hh * 100 + mm;
}

// hhmm + `minutes` (minutes < 60 assumed; used only for the +3min close delay).
inline int AddMinutesHhmm(int hhmm, int minutes) {
  int total = (hhmm / 100) * 60 + (hhmm % 100) + minutes;
  return (total / 60) * 100 + (total % 60);
}

enum class SessionPhase { kPreOpen, kContinuous, kClosingAuction, kClosed };

inline SessionPhase PhaseForHhmm(int hhmm) {
  if (hhmm < kOpenHhmm) return SessionPhase::kPreOpen;
  if (hhmm < kCloseWindowStartHhmm) return SessionPhase::kContinuous;
  if (hhmm < kCloseHhmm) return SessionPhase::kClosingAuction;
  return SessionPhase::kClosed;
}

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SIM_SESSION_SCHEDULE_H_
