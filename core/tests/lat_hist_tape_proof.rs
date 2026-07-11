//! Adversarial reviewer proof: run the committed trend_day tape through the REAL
//! decode path + the REAL latency histogram and print the resulting stats fields.
//! Demonstrates the line renders sanely with historical replay timestamps.

use std::path::PathBuf;

use kairos_core::decode::{FeedEvent, decode_feed_event};
use kairos_core::lat_hist::Histogram;
use kairos_core::replay::KqrSource;

#[test]
fn tape_latency_summary_renders_sanely() {
    let tape =
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/tapes/trend_day_2330.kqr");
    let src = KqrSource::open_files(&[tape]).unwrap();

    let hist: Vec<Histogram> = (0..8).map(|_| Histogram::default()).collect();
    let mut min_delta = i64::MAX;
    let mut max_delta = i64::MIN;
    for rec in src {
        let (source, venue, recv) = match decode_feed_event(&rec.payload) {
            Ok(FeedEvent::Quote(q)) => (q.source, q.quote_ts_us, q.recv_ts_us),
            Ok(FeedEvent::Trade(t)) => (t.source, t.trade_ts_us, t.recv_ts_us),
            Err(_) => continue,
        };
        min_delta = min_delta.min(recv - venue);
        max_delta = max_delta.max(recv - venue);
        hist[(source as usize).min(7)].observe(venue, recv);
    }

    let mut line = String::from(
        "kairos-core: quotes=0 (+0) decode_err=0 unknown_variant=0 clients=0 lagged=0",
    );
    for (i, h) in hist.iter().enumerate() {
        let s = h.snapshot_reset().summary();
        if s.n > 0 || s.missing > 0 {
            line.push_str(&format!(
                " lat[src{i},drift]: p50={}us p95={}us p99={}us max={}us n={} neg={} missing={}",
                s.p50, s.p95, s.p99, s.max, s.n, s.neg, s.missing
            ));
        }
    }
    println!("PROOF min_delta={min_delta} max_delta={max_delta}");
    println!("PROOF STATS LINE: {line}");
}
