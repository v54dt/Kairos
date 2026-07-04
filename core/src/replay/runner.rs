//! Runner: `drive_replay` pulls paced records and hands each to an injected offer
//! sink, maintaining the stats counters. Keeping the offer sink injected means the
//! reusable core carries no Aeron dependency — the daemon's sink owns the
//! publication map and backpressure retry; a test's sink can just collect bytes.

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU64, Ordering};

use super::pacer::Paced;
use super::source::ReplayRecord;

/// Result of one offer attempt reported back by the sink.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OfferOutcome {
    Offered,
    Dropped,
}

const NO_REPLAY_TS: i64 = i64::MIN;

/// Live replay counters. `first_replay_us`/`last_replay_us` track the virtual
/// replay clock for the stats line; per-stream counts index into a fixed vector
/// built from the known output-stream set.
pub struct ReplayStats {
    pub offered: AtomicU64,
    pub dropped: AtomicU64,
    pub bytes: AtomicU64,
    pub first_replay_us: AtomicI64,
    pub last_replay_us: AtomicI64,
    per_stream: Vec<(u32, AtomicU64)>,
    index: HashMap<u32, usize>,
}

impl ReplayStats {
    pub fn new(streams: &[u32]) -> Self {
        let mut per_stream = Vec::with_capacity(streams.len());
        let mut index = HashMap::new();
        for &s in streams {
            index.entry(s).or_insert_with(|| {
                per_stream.push((s, AtomicU64::new(0)));
                per_stream.len() - 1
            });
        }
        Self {
            offered: AtomicU64::new(0),
            dropped: AtomicU64::new(0),
            bytes: AtomicU64::new(0),
            first_replay_us: AtomicI64::new(NO_REPLAY_TS),
            last_replay_us: AtomicI64::new(NO_REPLAY_TS),
            per_stream,
            index,
        }
    }

    fn note_replay_now(&self, us: i64) {
        if self.first_replay_us.load(Ordering::Relaxed) == NO_REPLAY_TS {
            self.first_replay_us.store(us, Ordering::Relaxed);
        }
        self.last_replay_us.store(us, Ordering::Relaxed);
    }

    fn note_offered(&self, stream_id: u32, bytes: usize) {
        self.offered.fetch_add(1, Ordering::Relaxed);
        self.bytes.fetch_add(bytes as u64, Ordering::Relaxed);
        if let Some(&i) = self.index.get(&stream_id) {
            self.per_stream[i].1.fetch_add(1, Ordering::Relaxed);
        }
    }

    /// Per-stream offered counts, in the order the streams were registered.
    pub fn per_stream(&self) -> Vec<(u32, u64)> {
        self.per_stream
            .iter()
            .map(|(s, c)| (*s, c.load(Ordering::Relaxed)))
            .collect()
    }

    /// The recorded time span replayed so far, or `None` before the first record.
    pub fn replay_span_us(&self) -> Option<(i64, i64)> {
        let first = self.first_replay_us.load(Ordering::Relaxed);
        if first == NO_REPLAY_TS {
            return None;
        }
        Some((first, self.last_replay_us.load(Ordering::Relaxed)))
    }
}

/// Pull paced records until exhausted or `stop` is set, offering each via `sink`
/// and counting the outcome. The pacer already applied any required delay.
pub fn drive_replay<P>(
    mut paced: P,
    stop: &AtomicBool,
    stats: &ReplayStats,
    mut sink: impl FnMut(&ReplayRecord) -> OfferOutcome,
) where
    P: Iterator<Item = Paced>,
{
    while !stop.load(Ordering::Relaxed) {
        let Some(p) = paced.next() else { break };
        stats.note_replay_now(p.replay_now_us);
        match sink(&p.record) {
            OfferOutcome::Offered => stats.note_offered(p.record.stream_id, p.record.payload.len()),
            OfferOutcome::Dropped => {
                stats.dropped.fetch_add(1, Ordering::Relaxed);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::replay::pacer::{FakeClock, Pace, Pacer};
    use crate::replay::source::ReplayRecord;

    fn rec(stream_id: u32, ts: i64, payload: &[u8]) -> ReplayRecord {
        ReplayRecord {
            stream_id,
            recv_ts_us: ts,
            payload: payload.to_vec(),
        }
    }

    #[test]
    fn counts_offered_bytes_and_per_stream() {
        let records = vec![
            rec(1001, 10, b"aaaa"),
            rec(1002, 20, b"bb"),
            rec(1001, 30, b"cccccc"),
        ];
        let pacer = Pacer::new(records.into_iter(), FakeClock::new(), Pace::Full);
        let stats = ReplayStats::new(&[1001, 1002]);
        let stop = AtomicBool::new(false);
        drive_replay(pacer, &stop, &stats, |_| OfferOutcome::Offered);

        assert_eq!(stats.offered.load(Ordering::Relaxed), 3);
        assert_eq!(stats.dropped.load(Ordering::Relaxed), 0);
        assert_eq!(stats.bytes.load(Ordering::Relaxed), 12);
        assert_eq!(stats.per_stream(), vec![(1001, 2), (1002, 1)]);
        assert_eq!(stats.replay_span_us(), Some((10, 30)));
    }

    #[test]
    fn counts_drops() {
        let records = vec![rec(1001, 1, b"x"), rec(1001, 2, b"y")];
        let pacer = Pacer::new(records.into_iter(), FakeClock::new(), Pace::Full);
        let stats = ReplayStats::new(&[1001]);
        let stop = AtomicBool::new(false);
        drive_replay(pacer, &stop, &stats, |_| OfferOutcome::Dropped);

        assert_eq!(stats.offered.load(Ordering::Relaxed), 0);
        assert_eq!(stats.dropped.load(Ordering::Relaxed), 2);
        assert_eq!(stats.bytes.load(Ordering::Relaxed), 0);
    }

    #[test]
    fn stop_flag_halts_before_next() {
        let records = vec![rec(1001, 1, b"x"), rec(1001, 2, b"y")];
        let pacer = Pacer::new(records.into_iter(), FakeClock::new(), Pace::Full);
        let stats = ReplayStats::new(&[1001]);
        let stop = AtomicBool::new(true);
        drive_replay(pacer, &stop, &stats, |_| OfferOutcome::Offered);
        assert_eq!(stats.offered.load(Ordering::Relaxed), 0);
    }
}
