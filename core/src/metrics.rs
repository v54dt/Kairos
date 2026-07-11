//! Minimal core metrics: atomic counters + a 10s English stats line. Surfaces
//! what was previously swallowed (decode failures, broadcast lag) and proves the
//! core is alive. A Prometheus endpoint can layer on these later.
//!
//! The stats line also carries a per-source feed-latency summary (recv - venue
//! ts). That delta INCLUDES the exchange/broker-vs-host clock offset, so it is a
//! relative-drift / regression signal, not an absolute latency truth; the
//! histogram resets every interval.

use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;

use crate::lat_hist::{Histogram, LatSummary};

const STATS_INTERVAL: Duration = Duration::from_secs(10);

/// Feed-latency histograms are kept per source id; ids at or beyond this clamp
/// into the last slot (feeds use small dense source ids).
const N_SOURCES: usize = 8;

#[derive(Default)]
pub struct Metrics {
    pub quotes_decoded: AtomicU64,   // quotes decoded off Aeron
    pub decode_errors: AtomicU64,    // undecodable fragments (was silently dropped)
    pub unknown_variants: AtomicU64, // well-formed Envelopes of an unrouted variant
    pub clients: AtomicU64,          // connected UDS consumers (gauge)
    pub lagged: AtomicU64,           // broadcast lag events (a slow consumer fell behind)
    pub ordering_drops: AtomicU64,   // quotes dropped as out-of-order within a (source,symbol)
    lat: [Histogram; N_SOURCES],     // per-source recv-minus-venue latency (per-interval)
}

/// Cumulative counter values snapshotted for one stats line.
struct Counters {
    quotes: u64,
    dq: u64,
    decode_err: u64,
    unknown: u64,
    clients: u64,
    lagged: u64,
    ordering_drops: u64,
}

impl Metrics {
    pub fn inc(field: &AtomicU64) {
        field.fetch_add(1, Ordering::Relaxed);
    }

    /// Record one feed event's venue-to-here latency. O(1), alloc/lock-free —
    /// safe on the hot decode thread (the single writer).
    pub fn observe_latency(&self, source: u16, venue_ts_us: i64, recv_ts_us: i64) {
        let idx = (source as usize).min(N_SOURCES - 1);
        self.lat[idx].observe(venue_ts_us, recv_ts_us);
    }

    /// Spawns a thread that logs a cumulative stats line every 10s, including the
    /// quote delta over the interval and a per-source latency summary (reset each
    /// interval).
    pub fn spawn_logger(self: Arc<Self>) {
        std::thread::spawn(move || {
            let mut last_quotes = 0u64;
            loop {
                std::thread::sleep(STATS_INTERVAL);
                let quotes = self.quotes_decoded.load(Ordering::Relaxed);
                let counters = Counters {
                    quotes,
                    dq: quotes.saturating_sub(last_quotes),
                    decode_err: self.decode_errors.load(Ordering::Relaxed),
                    unknown: self.unknown_variants.load(Ordering::Relaxed),
                    clients: self.clients.load(Ordering::Relaxed),
                    lagged: self.lagged.load(Ordering::Relaxed),
                    ordering_drops: self.ordering_drops.load(Ordering::Relaxed),
                };
                let lat: Vec<(usize, LatSummary)> = self
                    .lat
                    .iter()
                    .enumerate()
                    .filter_map(|(i, h)| {
                        let s = h.snapshot_reset().summary();
                        (s.n > 0 || s.missing > 0).then_some((i, s))
                    })
                    .collect();
                eprintln!("{}", render_stats(&counters, &lat));
                last_quotes = quotes;
            }
        });
    }
}

/// Render the one-line stats string. Kept pure for unit testing. The latency
/// groups (venue-to-here, incl. clock offset) are appended per source and never
/// reorder the leading counters.
fn render_stats(c: &Counters, lat: &[(usize, LatSummary)]) -> String {
    let mut line = format!(
        "kairos-core: quotes={} (+{}) decode_err={} unknown_variant={} clients={} lagged={}",
        c.quotes, c.dq, c.decode_err, c.unknown, c.clients, c.lagged,
    );
    line.push_str(&format!(" drops={}", c.ordering_drops));
    for (src, s) in lat {
        line.push_str(&format!(
            " lat[src{src},drift]: p50={}us p95={}us p99={}us max={}us n={} neg={} missing={}",
            s.p50, s.p95, s.p99, s.max, s.n, s.neg, s.missing,
        ));
    }
    line
}

#[cfg(test)]
mod tests {
    use super::*;

    fn base_counters() -> Counters {
        Counters {
            quotes: 100,
            dq: 20,
            decode_err: 0,
            unknown: 0,
            clients: 1,
            lagged: 0,
            ordering_drops: 0,
        }
    }

    #[test]
    fn render_without_latency_matches_legacy_prefix() {
        let line = render_stats(&base_counters(), &[]);
        assert_eq!(
            line,
            "kairos-core: quotes=100 (+20) decode_err=0 unknown_variant=0 clients=1 lagged=0 drops=0"
        );
    }

    #[test]
    fn render_appends_drops_after_legacy_prefix() {
        let mut c = base_counters();
        c.ordering_drops = 7;
        let line = render_stats(&c, &[]);
        assert_eq!(
            line,
            "kairos-core: quotes=100 (+20) decode_err=0 unknown_variant=0 clients=1 lagged=0 drops=7"
        );
    }

    #[test]
    fn render_appends_latency_group_after_counters() {
        let s = LatSummary {
            p50: 128,
            p95: 1024,
            p99: 4096,
            max: 8192,
            n: 42,
            neg: 3,
            missing: 5,
        };
        let line = render_stats(&base_counters(), &[(0, s)]);
        assert!(line.starts_with(
            "kairos-core: quotes=100 (+20) decode_err=0 unknown_variant=0 clients=1 lagged=0"
        ));
        assert!(line.contains(
            " lat[src0,drift]: p50=128us p95=1024us p99=4096us max=8192us n=42 neg=3 missing=5"
        ));
    }

    #[test]
    fn observe_latency_clamps_out_of_range_source() {
        let m = Metrics::default();
        m.observe_latency(9999, 100, 200); // clamps into the last slot
        let s = m.lat[N_SOURCES - 1].snapshot_reset().summary();
        assert_eq!(s.n, 1);
    }
}
