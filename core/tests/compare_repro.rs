// Adversarial repro for feed_compare / compare module (A2c deliverable).
use kairos_core::compare::compare_streams;
use kairos_core::decode::FeedEvent;
use kairos_core::model::{Exchange, Session, Trade};

fn trade(source: u16, symbol: &str, mant: i64, scale: u8, ts: i64, vol: i64) -> Trade {
    Trade {
        symbol: symbol.to_owned(),
        exchange: Exchange::Twse,
        source,
        seq: 0,
        epoch: 0,
        trade_ts_us: ts,
        recv_ts_us: 0,
        price_mantissa: mant,
        price_scale: scale,
        volume: vol,
        is_trial: false,
        session: Session::Unknown,
        trading_date: 0,
        simtrade: false,
        underlying_price: 0,
    }
}

// (1) A source dropping a trade the other source saw is a real cross-source
// divergence; the count-aware compare flags it instead of truncating to the
// shorter stream.
#[test]
fn missing_trade_on_one_source_is_flagged() {
    let a = vec![
        FeedEvent::Trade(trade(0, "2330", 58050, 2, 1000, 1)),
        FeedEvent::Trade(trade(0, "2330", 58100, 2, 2000, 1)), // B never saw this
    ];
    let b = vec![FeedEvent::Trade(trade(1, "2330", 58050, 2, 1000, 1))];
    let report = compare_streams(&a, &b);
    assert!(report.trade_mismatches.is_empty());
    assert_eq!(report.trade_count_mismatches.len(), 1);
    assert!(!report.is_clean());
}

// (2) A corrupt / adversarial priceScale (u8 up to 255) is handled with
// checked_pow, so price_eq treats an unrepresentable scale as "cannot prove
// different" rather than overflowing (panic in a checked build, wrap in release).
#[test]
fn huge_price_scale_does_not_panic_the_compare() {
    let a = vec![FeedEvent::Trade(trade(0, "2330", 1, 39, 1000, 1))];
    let b = vec![FeedEvent::Trade(trade(1, "2330", 1, 2, 1000, 1))];
    let r = std::panic::catch_unwind(|| compare_streams(&a, &b));
    assert!(r.is_ok(), "compare_streams must not panic on scale=39");
}

// (3) feed_compare reads production KQR recordings, which frequently have a torn
// tail (recorder killed at EOD, not gracefully stopped). A single torn tail
// record makes read_kqr_events abort the WHOLE file instead of comparing the
// valid records before it.
#[test]
fn torn_tail_aborts_whole_recording() {
    use kairos_core::compare::read_kqr_events;
    use kairos_core::encode::encode_trade;
    use kairos_core::record::{FileHeader, RecordWriter};
    let mut w = RecordWriter::create(Vec::new(), &FileHeader::new(1001, 0)).unwrap();
    w.append(0, &encode_trade(&trade(0, "2330", 58050, 2, 1000, 1)))
        .unwrap();
    w.append(1, &encode_trade(&trade(0, "2330", 58100, 2, 2000, 1)))
        .unwrap();
    let mut buf = w.into_inner();
    // simulate a crash mid-write: drop the last few bytes of the final record
    buf.truncate(buf.len() - 3);
    let r = read_kqr_events(&buf);
    // The two fully-written records are unrecoverable to the tool.
    assert!(
        r.is_err(),
        "expected torn-tail to surface as Err (whole file lost)"
    );
}
