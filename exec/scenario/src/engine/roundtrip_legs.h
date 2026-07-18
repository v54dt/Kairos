#ifndef KAIROS_EXEC_ROUNDTRIP_LEGS_H_
#define KAIROS_EXEC_ROUNDTRIP_LEGS_H_

// Pure (SDK-free) derivation of the two round-trip legs from a parent scenario.
// The RoundTripRunner (PR4) drives timing/signals; this file only shapes the
// enter/exit sub-scenarios so they stay unit-testable.

#include "scenario.h"

namespace kairos::exec {

// Same-day forced-exit wall time (13:25) in minutes since midnight.
constexpr int kForcedExitMin = 13 * 60 + 25;

// HHMM (e.g. 1305) -> minutes since midnight. HHMM is non-linear across the hour
// boundary, so callers must convert before any minute arithmetic.
inline int HhmmToMinutes(int hhmm) { return (hhmm / 100) * 60 + hhmm % 100; }

inline int MinutesToHhmm(int minutes) { return (minutes / 60) * 100 + minutes % 60; }

// Why the exit leg fires: reverse-signal keeps the parent pricing policy, every
// other reason crosses the spread to close now.
enum class ExitReason { kReverseSignal, kStopLoss, kHoldTimeout, kForcedTime };

// Enter leg: a Buy of the parent budget over [trigger_min, trigger_min +
// enter_window_min], clamped to the 13:25 forced exit, parent pricing untouched.
Scenario DeriveEnterLeg(const Scenario& parent, int trigger_min);

// Exit leg: a Sell of exactly entered_shares as a same-day close (fees.daytrade),
// windowed [now_min, 13:25]; pricing per reason.
Scenario DeriveExitLeg(const Scenario& parent, long entered_shares, ExitReason reason, int now_min);

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ROUNDTRIP_LEGS_H_
