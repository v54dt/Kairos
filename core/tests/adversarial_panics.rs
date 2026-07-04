//! Regression: malformed KQR records (large priceScale, seq near u64::MAX) must
//! not make the offline compare tool overflow — no panic in a checked (debug)
//! build, no silent wrap in release. The tool ingests untrusted recordings, so
//! adversarial scale/seq values are handled without arithmetic overflow.

use kairos_core::compare::{compare_streams, find_seq_gaps};
use kairos_core::decode::FeedEvent;
use kairos_core::model::{Exchange, PriceLevel, Quote, QuoteBoard, Session, Trade};

fn trade_scaled(source: u16, mant: i64, scale: u8) -> Trade {
    Trade {
        symbol: "2330".to_owned(),
        exchange: Exchange::Twse,
        source,
        seq: 1,
        epoch: 1,
        trade_ts_us: 0,
        recv_ts_us: 0,
        price_mantissa: mant,
        price_scale: scale,
        volume: 1,
        is_trial: false,
        session: Session::Unknown,
        trading_date: 0,
        simtrade: false,
        underlying_price: 0,
    }
}

fn quote_seq(seq: u64) -> Quote {
    Quote {
        symbol: "2330".to_owned(),
        exchange: Exchange::Twse,
        quote_ts_us: 0,
        bids: vec![PriceLevel {
            price_mantissa: 1,
            price_scale: 2,
            volume: 1,
        }],
        asks: vec![PriceLevel {
            price_mantissa: 2,
            price_scale: 2,
            volume: 1,
        }],
        last_price: 0,
        last_scale: 0,
        last_volume: 0,
        is_trial: false,
        source: 0,
        seq,
        epoch: 1,
        recv_ts_us: 0,
        board: QuoteBoard::RoundLot,
        session: Session::Unknown,
        trading_date: 0,
        simtrade: false,
        underlying_price: 0,
    }
}

// price_eq() computes 10i128.checked_pow(scale) before checked_mul, so a corrupt
// recording with priceScale=40 (>= 39 overflows i128) is handled as "cannot
// prove different" instead of overflowing.
#[test]
fn large_price_scale_does_not_overflow_compare() {
    let a = vec![FeedEvent::Trade(trade_scaled(0, 5, 40))];
    let b = vec![FeedEvent::Trade(trade_scaled(1, 5, 40))];
    let report = compare_streams(&a, &b);
    assert!(report.trade_mismatches.is_empty());
}

// find_seq_gaps() uses prev_seq.saturating_add(1). seq == u64::MAX then another
// same-key record does not overflow and does not spuriously report a gap.
#[test]
fn seq_near_max_does_not_overflow_gap_detector() {
    let events = vec![
        FeedEvent::Quote(quote_seq(u64::MAX)),
        FeedEvent::Quote(quote_seq(5)),
    ];
    let gaps = find_seq_gaps(&events);
    assert!(gaps.is_empty());
}
