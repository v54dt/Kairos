//! Minimal core metrics: atomic counters + a 10s English stats line. Surfaces
//! what was previously swallowed (decode failures, broadcast lag) and proves the
//! core is alive. A Prometheus endpoint can layer on these later.

use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;

const STATS_INTERVAL: Duration = Duration::from_secs(10);

#[derive(Default)]
pub struct Metrics {
    pub quotes_decoded: AtomicU64,   // quotes decoded off Aeron
    pub decode_errors: AtomicU64,    // undecodable fragments (was silently dropped)
    pub unknown_variants: AtomicU64, // well-formed Envelopes of an unrouted variant
    pub clients: AtomicU64,          // connected UDS consumers (gauge)
    pub lagged: AtomicU64,           // broadcast lag events (a slow consumer fell behind)
}

impl Metrics {
    pub fn inc(field: &AtomicU64) {
        field.fetch_add(1, Ordering::Relaxed);
    }

    /// Spawns a thread that logs a cumulative stats line every 10s, including the
    /// quote delta over the interval (a live rate indicator).
    pub fn spawn_logger(self: Arc<Self>) {
        std::thread::spawn(move || {
            let mut last_quotes = 0u64;
            loop {
                std::thread::sleep(STATS_INTERVAL);
                let quotes = self.quotes_decoded.load(Ordering::Relaxed);
                eprintln!(
                    "kairos-core: quotes={quotes} (+{}) decode_err={} unknown_variant={} clients={} lagged={}",
                    quotes.saturating_sub(last_quotes),
                    self.decode_errors.load(Ordering::Relaxed),
                    self.unknown_variants.load(Ordering::Relaxed),
                    self.clients.load(Ordering::Relaxed),
                    self.lagged.load(Ordering::Relaxed),
                );
                last_quotes = quotes;
            }
        });
    }
}
