//! Primary/secondary source failover so UDS clients keep receiving ONE coherent
//! stream of quotes per symbol even with two broker feeds. Core serves the
//! primary source's book; if the primary goes stale WHILE a secondary is
//! receiving, it fails over to the secondary, and fails back to the primary only
//! after the primary has been continuously fresh for a hold window (hysteresis,
//! so a flickering feed cannot flap the selection).
//!
//! Staleness is RELATIVE and per-source-global (v1): the primary is only
//! considered failed when it is silent past the stale window AND a secondary is
//! currently receiving. A market-wide quiet period (every source silent) never
//! triggers a switch. Best-of-book arbitration is out of scope.
//!
//! With fewer than two sources the selector is inert: `serves` is always true,
//! `eval` never switches, and `note` is a no-op — behavior is byte-identical to
//! the single-feed pipeline.

use std::sync::atomic::{AtomicU16, AtomicU64, Ordering};
use std::time::Instant;

const DEFAULT_STALE_MS: u64 = 2_000;
const DEFAULT_RECOVER_HOLD_MS: u64 = 5_000;

/// A change of the active serving source.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Switch {
    pub from: u16,
    pub to: u16,
}

pub struct Selector {
    single: bool,
    priority: Vec<u16>,
    last_us: Vec<AtomicU64>,
    active: AtomicU16,
    prim_healthy_since: AtomicU64,
    stale_us: u64,
    recover_hold_us: u64,
    start: Instant,
}

impl Selector {
    pub fn new(priority: Vec<u16>, stale_us: u64, recover_hold_us: u64) -> Self {
        let active = priority.first().copied().unwrap_or(0);
        let last_us = priority.iter().map(|_| AtomicU64::new(0)).collect();
        Self {
            single: priority.len() < 2,
            priority,
            last_us,
            active: AtomicU16::new(active),
            prim_healthy_since: AtomicU64::new(0),
            stale_us,
            recover_hold_us,
            start: Instant::now(),
        }
    }

    /// Build from a priority list and the `KAIROS_FAILOVER_STALE_MS` /
    /// `KAIROS_FAILOVER_RECOVER_HOLD_MS` env knobs.
    pub fn from_env(priority: Vec<u16>) -> Self {
        let stale_ms = env_ms("KAIROS_FAILOVER_STALE_MS", DEFAULT_STALE_MS);
        let recover_ms = env_ms("KAIROS_FAILOVER_RECOVER_HOLD_MS", DEFAULT_RECOVER_HOLD_MS);
        Self::new(priority, stale_ms * 1_000, recover_ms * 1_000)
    }

    /// Whether more than one source participates (failover is active).
    pub fn is_multi(&self) -> bool {
        !self.single
    }

    /// Monotonic microseconds since construction; the caller stamps `note`/`eval`
    /// with this so tests can inject a controlled clock.
    pub fn now_us(&self) -> u64 {
        self.start.elapsed().as_micros() as u64
    }

    fn pos(&self, source: u16) -> Option<usize> {
        self.priority.iter().position(|&s| s == source)
    }

    /// Record that `source` produced an event at `now_us`. Lock-free; a no-op with
    /// a single source.
    pub fn note(&self, source: u16, now_us: u64) {
        if self.single {
            return;
        }
        if let Some(i) = self.pos(source) {
            self.last_us[i].store(now_us, Ordering::Relaxed);
        }
    }

    /// Whether `source` is the one currently served to clients. Always true with a
    /// single source (so the publish path is byte-identical to today).
    pub fn serves(&self, source: u16) -> bool {
        self.single || source == self.active.load(Ordering::Relaxed)
    }

    pub fn active_source(&self) -> u16 {
        self.active.load(Ordering::Relaxed)
    }

    /// The order in which to resolve a snapshot: the active source first, then the
    /// remaining sources by priority (a fallback for a symbol the active source has
    /// not yet booked). With a single source this is just that source.
    pub fn serve_order(&self) -> Vec<u16> {
        if self.single {
            return self.priority.clone();
        }
        let active = self.active_source();
        let mut order = Vec::with_capacity(self.priority.len());
        order.push(active);
        order.extend(self.priority.iter().copied().filter(|&s| s != active));
        order
    }

    fn fresh(&self, i: usize, now_us: u64) -> bool {
        let t = self.last_us[i].load(Ordering::Relaxed);
        if t == 0 {
            // Never emitted yet: grant the same stale window from startup so a
            // late-connecting source is not treated as stale-long-ago at t=0 (else a
            // cold primary fails over on the first tick before its first quote).
            return now_us <= self.stale_us;
        }
        now_us.saturating_sub(t) <= self.stale_us
    }

    fn first_fresh_from(&self, start_idx: usize, now_us: u64) -> Option<u16> {
        (start_idx..self.priority.len())
            .find(|&i| self.fresh(i, now_us))
            .map(|i| self.priority[i])
    }

    /// Recompute the active source. Returns `Some(Switch)` iff it changed. Intended
    /// to be called from ONE thread on a periodic tick.
    pub fn eval(&self, now_us: u64) -> Option<Switch> {
        if self.single {
            return None;
        }
        let primary = self.priority[0];
        let prim_fresh = self.fresh(0, now_us);
        if prim_fresh {
            let _ = self.prim_healthy_since.compare_exchange(
                0,
                now_us.max(1),
                Ordering::Relaxed,
                Ordering::Relaxed,
            );
        } else {
            self.prim_healthy_since.store(0, Ordering::Relaxed);
        }

        let active = self.active.load(Ordering::Relaxed);
        let target = if active == primary {
            if prim_fresh {
                primary
            } else {
                // Primary silent: fail over to the first receiving secondary, else
                // hold on the primary (market-wide quiet must not switch).
                self.first_fresh_from(1, now_us).unwrap_or(primary)
            }
        } else {
            let recovered = {
                let hs = self.prim_healthy_since.load(Ordering::Relaxed);
                hs != 0 && now_us.saturating_sub(hs) >= self.recover_hold_us
            };
            let active_fresh = self.pos(active).is_some_and(|i| self.fresh(i, now_us));
            if recovered {
                primary
            } else if active_fresh {
                active
            } else {
                // The serving secondary died before the primary earned switch-back:
                // move to the next fresh secondary, else hold. We do NOT jump to a
                // merely-momentarily-fresh primary here — that would flap tick-by-tick
                // when both feeds flicker in antiphase; the primary is taken only via
                // the recover-hold branch above.
                self.first_fresh_from(1, now_us).unwrap_or(active)
            }
        };

        if target != active {
            self.active.store(target, Ordering::Relaxed);
            Some(Switch {
                from: active,
                to: target,
            })
        } else {
            None
        }
    }
}

fn env_ms(key: &str, default: u64) -> u64 {
    std::env::var(key)
        .ok()
        .and_then(|v| v.parse::<u64>().ok())
        .filter(|&v| v > 0)
        .unwrap_or(default)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn single_source_is_inert() {
        let s = Selector::new(vec![0], 1_000, 5_000);
        assert!(!s.is_multi());
        assert!(s.serves(0));
        // A single source serves everything and never switches, regardless of ts.
        assert!(s.serves(7));
        s.note(0, 10_000);
        assert_eq!(s.eval(10_000_000), None);
        assert_eq!(s.active_source(), 0);
    }

    #[test]
    fn primary_stale_fails_over_to_secondary() {
        let s = Selector::new(vec![0, 1], 1_000, 5_000);
        s.note(0, 0);
        s.note(1, 0);
        assert_eq!(s.eval(500), None); // both fresh, stay on primary
        // Primary goes silent; secondary keeps receiving.
        s.note(1, 2_000);
        assert_eq!(s.eval(2_000), Some(Switch { from: 0, to: 1 }));
        assert!(s.serves(1));
        assert!(!s.serves(0));
        assert_eq!(s.active_source(), 1);
    }

    #[test]
    fn recovery_switches_back_only_after_hold() {
        let s = Selector::new(vec![0, 1], 1_000, 5_000);
        s.note(0, 0);
        s.note(1, 0);
        s.note(1, 2_000);
        assert_eq!(s.eval(2_000), Some(Switch { from: 0, to: 1 }));
        // Primary recovers at t=3000; both stay fresh through the hold window.
        for t in [3_000u64, 5_000, 7_000] {
            s.note(0, t);
            s.note(1, t);
            assert_eq!(
                s.eval(t),
                None,
                "must hold on secondary until the hold elapses"
            );
            assert_eq!(s.active_source(), 1);
        }
        // At t=8000 the primary has been fresh continuously since t=3000 (5000us).
        s.note(0, 8_000);
        s.note(1, 8_000);
        assert_eq!(s.eval(8_000), Some(Switch { from: 1, to: 0 }));
        assert_eq!(s.active_source(), 0);
    }

    #[test]
    fn alternating_primary_at_boundary_does_not_flap() {
        let s = Selector::new(vec![0, 1], 1_000, 5_000);
        s.note(0, 0);
        s.note(1, 0);
        let mut switches = 0usize;
        // Secondary steady; primary flickers stale at the boundary so it never earns
        // switch-back. Exactly one switch (primary -> secondary) must occur.
        let mut t = 1_500u64;
        for step in 0..20 {
            s.note(1, t); // secondary steady every step
            if step % 2 == 0 {
                s.note(0, t); // primary alive on even steps only
            }
            if s.eval(t).is_some() {
                switches += 1;
            }
            t += 1_500;
        }
        assert_eq!(
            switches, 1,
            "a flickering primary must not flap the selection"
        );
        assert_eq!(s.active_source(), 1);
    }

    #[test]
    fn market_wide_quiet_stays_on_primary() {
        let s = Selector::new(vec![0, 1], 1_000, 5_000);
        s.note(0, 0);
        s.note(1, 0);
        // Both go silent; no secondary is receiving, so no failover.
        assert_eq!(s.eval(100_000), None);
        assert_eq!(s.active_source(), 0);
    }

    #[test]
    fn dead_secondary_holds_until_primary_earns_recover_hold() {
        let s = Selector::new(vec![0, 1], 1_000, 5_000);
        s.note(0, 0);
        s.note(1, 0);
        s.note(1, 2_000);
        assert_eq!(s.eval(2_000), Some(Switch { from: 0, to: 1 }));
        // Secondary dies while the primary is receiving again but has NOT earned the
        // hold-based switch-back. Hold on the last coherent source rather than flap to
        // a momentarily-fresh primary.
        s.note(0, 4_000);
        assert_eq!(s.eval(4_000), None);
        assert_eq!(s.active_source(), 1);
        s.note(0, 6_000);
        assert_eq!(s.eval(6_000), None);
        assert_eq!(s.active_source(), 1);
        // Primary continuously fresh since 4_000; at 9_000 the hold has elapsed.
        s.note(0, 9_000);
        assert_eq!(s.eval(9_000), Some(Switch { from: 1, to: 0 }));
        assert_eq!(s.active_source(), 0);
    }

    #[test]
    fn late_primary_gets_startup_grace() {
        let s = Selector::new(vec![0, 1], 2_000_000, 5_000_000);
        // Secondary wins the first-quote race; the primary has not emitted yet.
        s.note(1, 100_000);
        assert_eq!(
            s.eval(100_000),
            None,
            "a never-noted primary keeps its stale window from startup"
        );
        assert_eq!(s.active_source(), 0);
        // Primary still silent past the stale window -> now fail over.
        s.note(1, 2_500_000);
        assert_eq!(
            s.eval(2_500_000),
            Some(Switch { from: 0, to: 1 }),
            "grace expires after the stale window"
        );
    }

    #[test]
    fn dead_secondary_moves_to_another_live_secondary() {
        let s = Selector::new(vec![0, 1, 2], 1_000, 5_000);
        s.note(0, 0);
        s.note(1, 0);
        s.note(2, 0);
        s.note(1, 2_000); // primary silent, first secondary receiving
        assert_eq!(s.eval(2_000), Some(Switch { from: 0, to: 1 }));
        // The serving secondary dies while another secondary is live: move to it
        // immediately (this is not a primary flap).
        s.note(2, 4_000);
        assert_eq!(s.eval(4_000), Some(Switch { from: 1, to: 2 }));
        assert_eq!(s.active_source(), 2);
    }

    #[test]
    fn antiphase_flicker_does_not_flap() {
        let s = Selector::new(vec![0, 1], 1_000, 5_000);
        s.note(0, 0);
        s.note(1, 0);
        let mut switches = 0usize;
        // Both feeds flicker stale/fresh in antiphase across eval ticks (primary on
        // even steps, secondary on odd). Without emergency-fallback hysteresis this
        // switched on every tick; it must settle after at most the first failover.
        let mut t = 1_500u64;
        for step in 0..20 {
            if step % 2 == 0 {
                s.note(0, t);
            } else {
                s.note(1, t);
            }
            if s.eval(t).is_some() {
                switches += 1;
            }
            t += 1_500;
        }
        assert!(
            switches <= 1,
            "antiphase flicker must not flap the selection: {switches} switches"
        );
    }
}
