//! Pacing engine: a pure, clock-injectable iterator that turns a stream of
//! recorded fragments into `(record, replay_now_us)` pairs, sleeping only as much
//! as the chosen pace requires. The sleeping is a side effect inside `next()` so
//! the daemon's offer loop stays trivial.
//!
//! - realtime (等速): preserve `recv_ts` deltas, anchored at the first record.
//! - accelerated (加速): same, divided by a `speed` factor (>1 faster).
//! - full-speed (全速+虛擬時鐘): never sleep; `replay_now_us` is the virtual clock
//!   downstream consumers read, jumping to each record's `recv_ts_us`.
//!
//! `replay_now_us` always equals the record's `recv_ts_us` for every pace, so
//! replayed-time tracks recorded time regardless of wall pacing.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use super::source::ReplayRecord;

/// Injectable time source (mirrors the B6 clock-seam pattern). `now_us` is a
/// monotonic microsecond reading; `sleep_for` blocks for the given duration.
pub trait Clock {
    fn now_us(&self) -> i64;
    fn sleep_for(&self, d: Duration);
}

/// Production clock: monotonic since construction, with a stop-aware chunked sleep
/// so Ctrl-C stays responsive even across a long recorded gap.
pub struct SystemClock {
    epoch: Instant,
    stop: Option<Arc<AtomicBool>>,
}

impl SystemClock {
    pub fn new(stop: Option<Arc<AtomicBool>>) -> Self {
        Self {
            epoch: Instant::now(),
            stop,
        }
    }
}

impl Clock for SystemClock {
    fn now_us(&self) -> i64 {
        self.epoch.elapsed().as_micros() as i64
    }

    fn sleep_for(&self, d: Duration) {
        let deadline = Instant::now() + d;
        let chunk = Duration::from_millis(50);
        loop {
            if let Some(s) = &self.stop
                && s.load(Ordering::Relaxed)
            {
                return;
            }
            let now = Instant::now();
            if now >= deadline {
                return;
            }
            std::thread::sleep((deadline - now).min(chunk));
        }
    }
}

/// The three replay paces. `Accel` accepts any positive factor.
#[derive(Debug, Clone, Copy)]
pub enum Pace {
    Realtime,
    Accel(f64),
    Full,
}

enum Mode {
    Timed { speed: f64 },
    Full,
}

/// One paced record: the fragment plus the virtual replay clock reading it maps to.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Paced {
    pub record: ReplayRecord,
    pub replay_now_us: i64,
}

/// Wraps a record iterator and paces it under an injected clock.
pub struct Pacer<I, C> {
    inner: I,
    clock: C,
    mode: Mode,
    anchor: Option<(i64, i64)>, // (wall_start_us, first_recv_ts_us)
}

impl<I, C> Pacer<I, C> {
    pub fn new(inner: I, clock: C, pace: Pace) -> Self {
        let mode = match pace {
            Pace::Realtime => Mode::Timed { speed: 1.0 },
            Pace::Accel(speed) => Mode::Timed { speed },
            Pace::Full => Mode::Full,
        };
        Self {
            inner,
            clock,
            mode,
            anchor: None,
        }
    }
}

impl<I, C> Iterator for Pacer<I, C>
where
    I: Iterator<Item = ReplayRecord>,
    C: Clock,
{
    type Item = Paced;

    fn next(&mut self) -> Option<Paced> {
        let record = self.inner.next()?;
        let replay_now_us = record.recv_ts_us;
        if let Mode::Timed { speed } = self.mode {
            let (wall_start, first_ts) = match self.anchor {
                Some(a) => a,
                None => {
                    let a = (self.clock.now_us(), record.recv_ts_us);
                    self.anchor = Some(a);
                    a
                }
            };
            let offset = ((record.recv_ts_us - first_ts) as f64 / speed).round() as i64;
            let sleep = (wall_start + offset) - self.clock.now_us();
            if sleep > 0 {
                self.clock.sleep_for(Duration::from_micros(sleep as u64));
            }
        }
        Some(Paced {
            record,
            replay_now_us,
        })
    }
}

#[cfg(test)]
pub(crate) struct FakeClock {
    now: std::cell::Cell<i64>,
    pub sleeps: std::rc::Rc<std::cell::RefCell<Vec<i64>>>,
}

#[cfg(test)]
impl FakeClock {
    pub fn new() -> Self {
        Self {
            now: std::cell::Cell::new(0),
            sleeps: std::rc::Rc::new(std::cell::RefCell::new(Vec::new())),
        }
    }
}

#[cfg(test)]
impl Clock for FakeClock {
    fn now_us(&self) -> i64 {
        self.now.get()
    }
    fn sleep_for(&self, d: Duration) {
        let us = d.as_micros() as i64;
        self.sleeps.borrow_mut().push(us);
        self.now.set(self.now.get() + us);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn recs(ts: &[i64]) -> Vec<ReplayRecord> {
        ts.iter()
            .map(|&t| ReplayRecord {
                stream_id: 1001,
                recv_ts_us: t,
                payload: vec![t as u8],
            })
            .collect()
    }

    fn run(ts: &[i64], pace: Pace) -> (Vec<i64>, Vec<i64>) {
        let clock = FakeClock::new();
        let sleeps = clock.sleeps.clone();
        let paced: Vec<Paced> = Pacer::new(recs(ts).into_iter(), clock, pace).collect();
        let replay_now: Vec<i64> = paced.iter().map(|p| p.replay_now_us).collect();
        (replay_now, sleeps.borrow().clone())
    }

    #[test]
    fn realtime_preserves_deltas() {
        let (replay_now, sleeps) = run(&[0, 10, 20], Pace::Realtime);
        assert_eq!(replay_now, vec![0, 10, 20]);
        assert_eq!(sleeps, vec![10, 10]);
    }

    #[test]
    fn accel_halves_sleep() {
        let (_, sleeps) = run(&[0, 10, 20], Pace::Accel(2.0));
        assert_eq!(sleeps, vec![5, 5]);
    }

    #[test]
    fn speed_rounds_to_nearest_micro() {
        // 10us / 3 = 3.33 -> 3
        let (_, sleeps) = run(&[0, 10], Pace::Accel(3.0));
        assert_eq!(sleeps, vec![3]);
    }

    #[test]
    fn full_speed_never_sleeps_and_tracks_recv_ts() {
        let (replay_now, sleeps) = run(&[100, 250, 900], Pace::Full);
        assert_eq!(replay_now, vec![100, 250, 900]);
        assert!(sleeps.is_empty());
    }

    #[test]
    fn identical_ts_produces_no_sleep() {
        let (_, sleeps) = run(&[5, 5, 5], Pace::Realtime);
        assert!(sleeps.is_empty());
    }

    #[test]
    fn ts_backwards_never_sleeps_negative() {
        // A backwards jump would compute a negative delay; it must be clamped, not
        // slept, and must not deadlock.
        let (replay_now, sleeps) = run(&[20, 10], Pace::Realtime);
        assert_eq!(replay_now, vec![20, 10]);
        assert!(sleeps.is_empty());
    }
}
