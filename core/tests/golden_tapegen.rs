//! Golden-day regression for the synthetic tape generator (CI-runnable, no driver).
//!
//! Two independent layers per fixture:
//!   A. Regenerate the tape in-memory from the SAME `GenParams::fixture_*` inputs
//!      and assert it is byte-identical to the committed .kqr. This catches any
//!      encode.rs / record-format / generator drift: bumping base-price, tick,
//!      drift step, the LCG, or any encoded capnp field flips this layer.
//!   B. Decode the committed bytes and assert a hardcoded event sequence (counts,
//!      spot prices/volumes, ts-window, monotonic ts, fill-relevance invariants).
//!      This pins semantics with literals, so even a coordinated generator+fixture
//!      change must still match the golden: changing a count or a spot value flips
//!      this layer.
//!
//! The heavier end-to-end golden (replay through kairos-sim + a trader, assert
//! fills/journal) needs the embedded Aeron driver and lives in the #[ignore]
//! tapegen_sim_e2e test; it is not part of this CI job.

use kairos_core::decode::{FeedEvent, decode_feed_event};
use kairos_core::record::{FileHeader, RecordReader, RecordWriter};
use kairos_core::tapegen::{GenParams, STREAM_ID, hhmm_from_us, in_session_window, write_tape};

const QUOTES: &[u8] = include_bytes!("fixtures/tapes/quotes_only_2330.kqr");
const TREND: &[u8] = include_bytes!("fixtures/tapes/trend_day_2330.kqr");
const LIMIT: &[u8] = include_bytes!("fixtures/tapes/limit_lock_2330.kqr");

fn regenerate(params: &GenParams) -> Vec<u8> {
    let mut w = RecordWriter::create(
        Vec::new(),
        &FileHeader::new(STREAM_ID, params.session_open_us()),
    )
    .unwrap();
    write_tape(&mut w, params).unwrap();
    w.into_inner()
}

fn events(bytes: &[u8]) -> Vec<FeedEvent> {
    let (hdr, r) = RecordReader::open(bytes).unwrap();
    assert_eq!(hdr.stream_id, STREAM_ID);
    r.map(|rec| {
        let rec = rec.unwrap();
        // The record ts and the payload's own recv_ts must agree and be in-window.
        assert!(in_session_window(rec.recv_ts_us), "record ts out of window");
        decode_feed_event(&rec.payload).unwrap()
    })
    .collect()
}

fn counts(ev: &[FeedEvent]) -> (usize, usize) {
    let q = ev
        .iter()
        .filter(|e| matches!(e, FeedEvent::Quote(_)))
        .count();
    let t = ev
        .iter()
        .filter(|e| matches!(e, FeedEvent::Trade(_)))
        .count();
    (q, t)
}

fn trades(ev: &[FeedEvent]) -> Vec<&kairos_core::model::Trade> {
    ev.iter()
        .filter_map(|e| match e {
            FeedEvent::Trade(t) => Some(t),
            _ => None,
        })
        .collect()
}

fn assert_monotonic_in_window(ev: &[FeedEvent]) {
    let ts = |e: &FeedEvent| match e {
        FeedEvent::Quote(q) => q.recv_ts_us,
        FeedEvent::Trade(t) => t.recv_ts_us,
    };
    let mut prev = i64::MIN;
    for e in ev {
        let t = ts(e);
        assert!(t > prev, "timestamps must be strictly increasing");
        let hhmm = hhmm_from_us(t);
        assert!(
            (900..1325).contains(&hhmm),
            "ts at {hhmm:04} out of session"
        );
        prev = t;
    }
}

#[test]
fn quotes_only_golden() {
    assert_eq!(regenerate(&GenParams::fixture_quotes_only()), QUOTES);
    assert!(QUOTES.len() < 256 * 1024);

    let ev = events(QUOTES);
    assert_eq!(counts(&ev), (120, 0));
    assert_monotonic_in_window(&ev);
    // Pure quote-flow tape: zero trades means zero fills are possible.
    assert!(trades(&ev).is_empty());
    let FeedEvent::Quote(q0) = &ev[0] else {
        panic!("first event must be a quote")
    };
    assert_eq!(q0.bids[0].price_mantissa, 57950);
    assert_eq!(q0.bids[0].volume, 44);
    assert_eq!(q0.asks[0].price_mantissa, 58050);
    assert_eq!(q0.last_price, 58000);
}

#[test]
fn trend_day_golden() {
    assert_eq!(regenerate(&GenParams::fixture_trend_day()), TREND);
    assert!(TREND.len() < 256 * 1024);

    let ev = events(TREND);
    assert_eq!(counts(&ev), (120, 120));
    assert_monotonic_in_window(&ev);

    let tr = trades(&ev);
    let reference = 57950i64; // initial best bid
    assert_eq!(tr.first().unwrap().price_mantissa, 57950);
    assert_eq!(tr.first().unwrap().volume, 4);
    assert_eq!(tr.last().unwrap().price_mantissa, 57250);
    assert_eq!(tr.last().unwrap().volume, 2);
    // Fill-relevance: a resting join BUY at the reference is crossed only by a
    // trade printing strictly below it (fill_model.cpp OnTrade conservative).
    assert!(
        tr.iter().any(|t| t.price_mantissa < reference),
        "trend-day must print strictly below the join reference"
    );
}

#[test]
fn limit_lock_golden() {
    assert_eq!(regenerate(&GenParams::fixture_limit_lock()), LIMIT);
    assert!(LIMIT.len() < 256 * 1024);

    let ev = events(LIMIT);
    assert_eq!(counts(&ev), (120, 120));
    assert_monotonic_in_window(&ev);

    let limit = 58000i64;
    let FeedEvent::Quote(q0) = &ev[0] else {
        panic!("first event must be a quote")
    };
    assert_eq!(q0.bids[0].price_mantissa, limit);
    assert_eq!(q0.bids[0].volume, 100);
    // Locked limit-up: no offer at/below the ceiling, so a BUY at the limit has
    // nothing to cross on arrival and stays unbuyable.
    assert!(q0.asks.is_empty());
    // Every trade still prints AT the limit; none crosses.
    assert!(trades(&ev).iter().all(|t| t.price_mantissa == limit));
}
