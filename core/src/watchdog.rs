//! Liveness watchdogs for the Aeron poll loop so a dead media driver or a
//! hard-erroring subscription makes kairos-core exit non-zero (systemd restarts
//! it) instead of spinning silently forever. Market silence is NOT a failure:
//! quiet nights/weekends produce `Ok(0)` polls and a live-but-idle driver, so
//! neither watchdog can trip on an absence of quotes.

use std::time::{Duration, Instant};

/// Consecutive hard-error budget before the poll loop is declared dead.
pub const DEFAULT_MAX_POLL_ERRORS: u32 = 64;
/// Staleness (ms) of the driver's own heartbeat past which it counts as inactive;
/// matches Aeron's default client driver-timeout so a sub-timeout bounce is tolerated.
pub const DEFAULT_DRIVER_TIMEOUT_MS: i64 = 10_000;
/// The driver must be observed continuously inactive for this long before exit, so a
/// single CnC read racing a driver restart cannot trip a false death.
pub const DRIVER_DEAD_GRACE: Duration = Duration::from_secs(3);

/// Trips after `threshold` consecutive poll errors. Any successful poll — including
/// an empty `Ok(0)` on a silent feed — resets the count, so silence never trips it.
pub struct PollErrorWatchdog {
    consecutive: u32,
    threshold: u32,
}

impl PollErrorWatchdog {
    pub fn new(threshold: u32) -> Self {
        Self {
            consecutive: 0,
            threshold,
        }
    }

    pub fn threshold(&self) -> u32 {
        self.threshold
    }

    pub fn on_ok(&mut self) {
        self.consecutive = 0;
    }

    /// Records one poll error; returns true once the consecutive count reaches the
    /// threshold (fatal).
    pub fn on_err(&mut self) -> bool {
        self.consecutive = self.consecutive.saturating_add(1);
        self.consecutive >= self.threshold
    }
}

/// Trips when the media driver is observed inactive continuously for `grace`. Driver
/// activity is the driver's own CnC heartbeat, updated regardless of quote flow, so a
/// live-but-idle driver (weekend) keeps this armed at zero.
pub struct DriverLivenessWatchdog {
    dead_since: Option<Instant>,
    grace: Duration,
}

impl DriverLivenessWatchdog {
    pub fn new(grace: Duration) -> Self {
        Self {
            dead_since: None,
            grace,
        }
    }

    /// Feeds one liveness observation. Returns true once the driver has been inactive
    /// continuously for at least `grace` (fatal); any active observation resets.
    pub fn observe(&mut self, active: bool, now: Instant) -> bool {
        if active {
            self.dead_since = None;
            return false;
        }
        let since = *self.dead_since.get_or_insert(now);
        now.duration_since(since) >= self.grace
    }
}

/// Consecutive poll-error budget from `$KAIROS_AERON_MAX_POLL_ERRORS`
/// (default `DEFAULT_MAX_POLL_ERRORS`; a non-numeric or zero value falls back).
pub fn max_poll_errors_from_env() -> u32 {
    std::env::var("KAIROS_AERON_MAX_POLL_ERRORS")
        .ok()
        .and_then(|v| v.parse::<u32>().ok())
        .filter(|&v| v > 0)
        .unwrap_or(DEFAULT_MAX_POLL_ERRORS)
}

/// Driver heartbeat-staleness threshold (ms) from `$KAIROS_DRIVER_TIMEOUT_MS`
/// (default `DEFAULT_DRIVER_TIMEOUT_MS`; a non-numeric or non-positive value falls back).
pub fn driver_timeout_ms_from_env() -> i64 {
    std::env::var("KAIROS_DRIVER_TIMEOUT_MS")
        .ok()
        .and_then(|v| v.parse::<i64>().ok())
        .filter(|&v| v > 0)
        .unwrap_or(DEFAULT_DRIVER_TIMEOUT_MS)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn poll_errors_trip_only_after_threshold() {
        let mut wd = PollErrorWatchdog::new(3);
        assert!(!wd.on_err());
        assert!(!wd.on_err());
        assert!(wd.on_err(), "third consecutive error is fatal");
    }

    #[test]
    fn a_single_success_resets_the_error_count() {
        let mut wd = PollErrorWatchdog::new(3);
        assert!(!wd.on_err());
        assert!(!wd.on_err());
        wd.on_ok();
        assert!(!wd.on_err());
        assert!(!wd.on_err());
        assert!(wd.on_err(), "count restarted from zero after the reset");
    }

    #[test]
    fn long_silent_but_healthy_feed_never_trips() {
        // A quiet weekend: every poll returns Ok(0) -> on_ok, never an error.
        let mut wd = PollErrorWatchdog::new(4);
        for _ in 0..1_000_000 {
            wd.on_ok();
        }
        assert!(!wd.on_err());
    }

    #[test]
    fn live_driver_never_trips_liveness() {
        // Weekend: driver alive and heartbeating, zero quotes flowing.
        let mut wd = DriverLivenessWatchdog::new(Duration::from_secs(3));
        let base = Instant::now();
        for s in 0..3600 {
            assert!(!wd.observe(true, base + Duration::from_secs(s)));
        }
    }

    #[test]
    fn dead_driver_trips_after_grace() {
        let mut wd = DriverLivenessWatchdog::new(Duration::from_secs(3));
        let base = Instant::now();
        assert!(
            !wd.observe(false, base),
            "first inactive read starts the clock"
        );
        assert!(!wd.observe(false, base + Duration::from_secs(2)));
        assert!(
            wd.observe(false, base + Duration::from_secs(3)),
            "inactive for the full grace window is fatal"
        );
    }

    #[test]
    fn one_active_read_resets_the_dead_clock() {
        let mut wd = DriverLivenessWatchdog::new(Duration::from_secs(3));
        let base = Instant::now();
        assert!(!wd.observe(false, base));
        assert!(!wd.observe(false, base + Duration::from_secs(2)));
        assert!(
            !wd.observe(true, base + Duration::from_secs(2)),
            "recovered"
        );
        // A later inactive read starts a fresh window, not resuming the old one.
        assert!(!wd.observe(false, base + Duration::from_secs(4)));
        assert!(!wd.observe(false, base + Duration::from_secs(6)));
        assert!(wd.observe(false, base + Duration::from_secs(7)));
    }

    #[test]
    fn env_defaults_apply_when_unset_or_invalid() {
        unsafe { std::env::remove_var("KAIROS_AERON_MAX_POLL_ERRORS") };
        assert_eq!(max_poll_errors_from_env(), DEFAULT_MAX_POLL_ERRORS);
        unsafe { std::env::set_var("KAIROS_AERON_MAX_POLL_ERRORS", "not-a-number") };
        assert_eq!(max_poll_errors_from_env(), DEFAULT_MAX_POLL_ERRORS);
        unsafe { std::env::set_var("KAIROS_AERON_MAX_POLL_ERRORS", "0") };
        assert_eq!(max_poll_errors_from_env(), DEFAULT_MAX_POLL_ERRORS);
        unsafe { std::env::set_var("KAIROS_AERON_MAX_POLL_ERRORS", "8") };
        assert_eq!(max_poll_errors_from_env(), 8);
        unsafe { std::env::remove_var("KAIROS_AERON_MAX_POLL_ERRORS") };

        unsafe { std::env::remove_var("KAIROS_DRIVER_TIMEOUT_MS") };
        assert_eq!(driver_timeout_ms_from_env(), DEFAULT_DRIVER_TIMEOUT_MS);
        unsafe { std::env::set_var("KAIROS_DRIVER_TIMEOUT_MS", "-5") };
        assert_eq!(driver_timeout_ms_from_env(), DEFAULT_DRIVER_TIMEOUT_MS);
        unsafe { std::env::set_var("KAIROS_DRIVER_TIMEOUT_MS", "20000") };
        assert_eq!(driver_timeout_ms_from_env(), 20_000);
        unsafe { std::env::remove_var("KAIROS_DRIVER_TIMEOUT_MS") };
    }
}
