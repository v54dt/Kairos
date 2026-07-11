//! Hand-rolled, fixed-size, no-deps log2 latency histogram. Measures
//! delta_us = recv_ts_us - venue_ts_us per feed event, i.e. venue-to-here.
//!
//! CAVEAT: this delta INCLUDES the offset between the exchange/broker clock and
//! our host clock — it is a relative-drift / regression signal, NOT an absolute
//! latency truth. A stable offset lands in one bucket; a jump shows up as drift.
//! Negative deltas (host clock behind venue, or a replayed historical tape) are
//! clamped into a dedicated non-positive bucket and counted, never cast to u64.
//! A missing/zero venue ts is counted separately and never touches the buckets.

use std::sync::atomic::{AtomicU64, Ordering};

/// Bucket 0 = non-positive delta; bucket k in 1..=64 = delta_us in [2^(k-1), 2^k).
/// A positive i64 delta's ilog2 is at most 62, so 65 buckets never overflow.
const N_BUCKETS: usize = 65;

pub struct Histogram {
    buckets: [AtomicU64; N_BUCKETS],
    missing: AtomicU64,
}

impl Default for Histogram {
    fn default() -> Self {
        Histogram {
            buckets: std::array::from_fn(|_| AtomicU64::new(0)),
            missing: AtomicU64::new(0),
        }
    }
}

/// Lower-bound microseconds represented by a bucket index (0 for the
/// non-positive bucket; 2^(idx-1) otherwise).
fn bucket_lower_us(idx: usize) -> u64 {
    if idx == 0 { 0 } else { 1u64 << (idx - 1) }
}

impl Histogram {
    /// O(1), alloc-free, lock-free: one relaxed increment on a fixed array.
    /// The single feed-decode thread is the only writer.
    pub fn observe(&self, venue_ts_us: i64, recv_ts_us: i64) {
        if venue_ts_us == 0 {
            self.missing.fetch_add(1, Ordering::Relaxed);
            return;
        }
        let delta = recv_ts_us - venue_ts_us;
        let idx = if delta <= 0 {
            0
        } else {
            1 + (delta as u64).ilog2() as usize
        };
        self.buckets[idx].fetch_add(1, Ordering::Relaxed);
    }

    /// Atomically drain the histogram for the just-finished interval. The single
    /// logger thread is the only reader; a concurrent `observe` lands in the next
    /// interval, so no event is lost.
    pub fn snapshot_reset(&self) -> Snapshot {
        let buckets = std::array::from_fn(|i| self.buckets[i].swap(0, Ordering::Relaxed));
        let missing = self.missing.swap(0, Ordering::Relaxed);
        Snapshot { buckets, missing }
    }
}

/// A drained per-interval view. All extraction is pure integer math on this.
pub struct Snapshot {
    buckets: [u64; N_BUCKETS],
    missing: u64,
}

/// Per-interval percentile summary in microseconds.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct LatSummary {
    pub p50: u64,
    pub p95: u64,
    pub p99: u64,
    pub max: u64,
    pub n: u64,       // events in the histogram (positive + non-positive), excl. missing
    pub neg: u64,     // non-positive-delta events (bucket 0)
    pub missing: u64, // events with a zero/absent venue ts (not bucketed)
}

impl Snapshot {
    pub fn count(&self) -> u64 {
        self.buckets.iter().sum()
    }

    /// Lower-bound us of the bucket at cumulative count >= ceil(pct/100 * n).
    /// The representative value is the bucket's lower bound (a documented
    /// approximation inherent to a log-bucketed histogram).
    pub fn percentile(&self, pct: u64) -> u64 {
        let n = self.count();
        if n == 0 {
            return 0;
        }
        let target = n.saturating_mul(pct).div_ceil(100).max(1);
        let mut cum = 0u64;
        for (idx, &c) in self.buckets.iter().enumerate() {
            cum += c;
            if cum >= target {
                return bucket_lower_us(idx);
            }
        }
        bucket_lower_us(N_BUCKETS - 1)
    }

    pub fn max_us(&self) -> u64 {
        for idx in (0..N_BUCKETS).rev() {
            if self.buckets[idx] > 0 {
                return bucket_lower_us(idx);
            }
        }
        0
    }

    pub fn summary(&self) -> LatSummary {
        LatSummary {
            p50: self.percentile(50),
            p95: self.percentile(95),
            p99: self.percentile(99),
            max: self.max_us(),
            n: self.count(),
            neg: self.buckets[0],
            missing: self.missing,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bucket_boundaries_are_adjacent() {
        let h = Histogram::default();
        // delta = 2^k - 1 lands in bucket k; delta = 2^k lands in bucket k+1.
        h.observe(1, 2); // delta 1 -> idx 1
        h.observe(1, 3); // delta 2 -> idx 2
        h.observe(1, 4); // delta 3 -> idx 2
        h.observe(1, 5); // delta 4 -> idx 3
        let s = h.snapshot_reset();
        assert_eq!(s.buckets[1], 1);
        assert_eq!(s.buckets[2], 2);
        assert_eq!(s.buckets[3], 1);
        assert_eq!(s.missing, 0);
    }

    #[test]
    fn power_of_two_edges() {
        let h = Histogram::default();
        // delta = 1024 (2^10) -> idx 11; delta = 1023 -> idx 10.
        h.observe(1, 1 + 1023);
        h.observe(1, 1 + 1024);
        let s = h.snapshot_reset();
        assert_eq!(s.buckets[10], 1);
        assert_eq!(s.buckets[11], 1);
        assert_eq!(bucket_lower_us(10), 512);
        assert_eq!(bucket_lower_us(11), 1024);
    }

    #[test]
    fn negative_and_zero_delta_go_to_nonpositive_bucket() {
        let h = Histogram::default();
        h.observe(100, 100); // delta 0
        h.observe(100, 50); // delta -50
        h.observe(100, 99); // delta -1
        let s = h.snapshot_reset();
        assert_eq!(s.buckets[0], 3);
        assert_eq!(s.count(), 3);
        assert_eq!(s.summary().neg, 3);
        assert_eq!(s.max_us(), 0);
    }

    #[test]
    fn missing_venue_ts_is_counted_not_bucketed() {
        let h = Histogram::default();
        h.observe(0, 12345);
        h.observe(0, 999);
        h.observe(50, 60); // one real delta
        let s = h.snapshot_reset();
        assert_eq!(s.missing, 2);
        assert_eq!(s.count(), 1); // buckets untouched by the missing events
        assert_eq!(s.summary().missing, 2);
    }

    #[test]
    fn percentile_exactness_on_known_distribution() {
        let h = Histogram::default();
        // 100 events: 90 with delta 1 (idx 1, lb 1), 9 with delta 1000
        // (ilog2=9 -> idx 10, lb 512), 1 with delta 1_000_000 (ilog2=19 ->
        // idx 20, lb 524288).
        for _ in 0..90 {
            h.observe(1, 2);
        }
        for _ in 0..9 {
            h.observe(1, 1001);
        }
        h.observe(1, 1_000_001);
        let s = h.snapshot_reset();
        let sm = s.summary();
        assert_eq!(sm.n, 100);
        assert_eq!(sm.p50, 1); // 50th -> the delta-1 mass
        assert_eq!(sm.p95, 512); // 95th -> into the delta-1000 mass
        assert_eq!(sm.p99, 512); // 99th -> still the delta-1000 mass
        assert_eq!(sm.max, 524_288); // the lone tail event
        assert_eq!(sm.neg, 0);
    }

    #[test]
    fn snapshot_reset_zeroes_and_next_interval_is_independent() {
        let h = Histogram::default();
        h.observe(1, 100);
        let s1 = h.snapshot_reset();
        assert_eq!(s1.count(), 1);
        let s2 = h.snapshot_reset();
        assert_eq!(s2.count(), 0);
        assert_eq!(s2.missing, 0);
        h.observe(1, 100);
        let s3 = h.snapshot_reset();
        assert_eq!(s3.count(), 1);
    }

    #[test]
    fn empty_snapshot_yields_zeroed_summary() {
        let s = Histogram::default().snapshot_reset();
        let sm = s.summary();
        assert_eq!(sm, LatSummary::default());
    }
}
