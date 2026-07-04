//! Adversarial repro tests for the A2 feed-compare consistency tool and the
//! seq/epoch gap detector. These probe the blind spots claimed by the review.

use kairos_core::compare::{compare_streams, find_seq_gaps};
use kairos_core::decode::FeedEvent;
use kairos_core::model::{Exchange, PriceLevel, Quote, QuoteBoard, Session, Trade};

fn quote(source: u16, symbol: &str, seq: u64, epoch: u32, bid: i64, ask: i64) -> Quote {
    Quote {
        symbol: symbol.to_owned(),
        exchange: Exchange::Twse,
        quote_ts_us: 0,
        bids: vec![PriceLevel {
            price_mantissa: bid,
            price_scale: 2,
            volume: 1,
        }],
        asks: vec![PriceLevel {
            price_mantissa: ask,
            price_scale: 2,
            volume: 1,
        }],
        last_price: 0,
        last_scale: 0,
        last_volume: 0,
        is_trial: false,
        source,
        seq,
        epoch,
        recv_ts_us: 0,
        board: QuoteBoard::RoundLot,
        session: Session::Unknown,
        trading_date: 0,
        simtrade: false,
        underlying_price: 0,
    }
}

fn trade(source: u16, symbol: &str, seq: u64, epoch: u32, mant: i64, ts: i64) -> Trade {
    Trade {
        symbol: symbol.to_owned(),
        exchange: Exchange::Twse,
        source,
        seq,
        epoch,
        trade_ts_us: ts,
        recv_ts_us: 0,
        price_mantissa: mant,
        price_scale: 2,
        volume: 1,
        is_trial: false,
        session: Session::Unknown,
        trading_date: 0,
        simtrade: false,
        underlying_price: 0,
    }
}

// Source A emitted 3 trades, source B only 2 (B dropped/never saw the 3rd). A
// consistency tool's whole job is to catch this; a count-aware compare flags it
// as a trade-count divergence instead of silently truncating to the shorter run.
#[test]
fn trade_count_divergence_is_flagged() {
    let a = vec![
        FeedEvent::Trade(trade(0, "2330", 1, 1, 58000, 100)),
        FeedEvent::Trade(trade(0, "2330", 2, 1, 58010, 200)),
        FeedEvent::Trade(trade(0, "2330", 3, 1, 58020, 300)), // B never has this
    ];
    let b = vec![
        FeedEvent::Trade(trade(1, "2330", 1, 1, 58000, 100)),
        FeedEvent::Trade(trade(1, "2330", 2, 1, 58010, 200)),
    ];
    let report = compare_streams(&a, &b);
    assert!(
        !report.is_clean(),
        "count divergence must not read as clean"
    );
    assert_eq!(report.trade_count_mismatches.len(), 1);
    assert_eq!(report.trade_count_mismatches[0].a_count, 3);
    assert_eq!(report.trade_count_mismatches[0].b_count, 2);
    assert!(report.trade_mismatches.is_empty());
}

// A trade dropped in the MIDDLE of B must NOT cascade into a run of fabricated
// per-index price mismatches; it surfaces once as a count divergence.
#[test]
fn single_middle_drop_reported_as_count_divergence_not_cascade() {
    let a = vec![
        FeedEvent::Trade(trade(0, "2330", 1, 1, 58000, 100)),
        FeedEvent::Trade(trade(0, "2330", 2, 1, 58010, 200)), // B dropped this one
        FeedEvent::Trade(trade(0, "2330", 3, 1, 58020, 300)),
        FeedEvent::Trade(trade(0, "2330", 4, 1, 58030, 400)),
    ];
    let b = vec![
        FeedEvent::Trade(trade(1, "2330", 1, 1, 58000, 100)),
        FeedEvent::Trade(trade(1, "2330", 3, 1, 58020, 300)),
        FeedEvent::Trade(trade(1, "2330", 4, 1, 58030, 400)),
    ];
    let report = compare_streams(&a, &b);
    assert_eq!(report.trade_count_mismatches.len(), 1);
    assert!(
        report.trade_mismatches.is_empty(),
        "one real drop must not fabricate per-index mismatches"
    );
}

// BLIND SPOT 2: a sidecar that never did an in-process rebuild (epoch stays 1)
// crashes and restarts. Its epoch counter is in-memory, so it comes back at 1
// again and seq resets to 1. Within a single continuously-recorded KQR file the
// gap detector sees (epoch 1, seq 5000) then (epoch 1, seq 1): same epoch, seq
// goes backwards -> NOT a gap, NOT an epoch change. The restart (and any data
// lost across it) is invisible, contradicting NORMALIZATION.md 3.
#[test]
fn process_restart_same_epoch_seq_reset_is_invisible() {
    let events = vec![
        FeedEvent::Quote(quote(0, "2330", 4999, 1, 58000, 58100)),
        FeedEvent::Quote(quote(0, "2330", 5000, 1, 58000, 58100)),
        // --- sidecar crash + restart here; epoch counter reset to 1 ---
        FeedEvent::Quote(quote(0, "2330", 1, 1, 58000, 58100)),
        FeedEvent::Quote(quote(0, "2330", 2, 1, 58000, 58100)),
    ];
    let gaps = find_seq_gaps(&events);
    assert!(
        gaps.is_empty(),
        "restart with reset-to-1 epoch was somehow detected"
    );
}
