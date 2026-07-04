//! Cross-language wire-format regression for the A2 append-only additions.
//!
//! Pins the NEW wire format: a Quote carrying the A2 fields (source/seq/epoch/
//! recvTsUs/board/session/...) and a standalone Trade event. The same two
//! fixtures are byte-matched by the C++ encoder tests
//! `sidecar/concords/tests/test_golden_v2.cpp` and `test_trade.cpp`, so any
//! cross-language drift in the new fields fails CI.
//!
//! Companion to `golden_wire.rs`, which pins the pre-A2 fixture and proves old
//! recordings still decode with capnp defaults.
//!
//! Known Quote in `quote_v2_golden_envelope.bin` (keep in sync with test_golden_v2.cpp):
//!   symbol=2330 exchange=Twse quote_ts_us=1_700_000_000_000_000
//!   bids=[{58000,2,100},{57950,2,50}] asks=[{58100,2,80}]
//!   last_price=58050 last_scale=2 last_volume=10 is_trial=false
//!   source=0 seq=12345 epoch=7 recv_ts_us=1_700_000_000_000_042
//!   board=RoundLot session=Day trading_date=20260704 simtrade=false underlying_price=0
//!
//! Known Trade in `trade_golden_envelope.bin` (keep in sync with test_trade.cpp):
//!   symbol=2330 exchange=Twse source=0 seq=12346 epoch=7
//!   trade_ts_us=1_700_000_000_000_100 recv_ts_us=1_700_000_000_000_142
//!   price_mantissa=58050 price_scale=2 volume=3 is_trial=false
//!   session=Day trading_date=20260704 simtrade=false underlying_price=0

use kairos_core::decode::{FeedEvent, decode_feed_event, decode_quote_bytes};
use kairos_core::model::{Exchange, QuoteBoard, Session};

const QUOTE_V2: &[u8] = include_bytes!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../schema/testdata/quote_v2_golden_envelope.bin"
));

const TRADE: &[u8] = include_bytes!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../schema/testdata/trade_golden_envelope.bin"
));

#[test]
fn golden_quote_v2_decodes_new_fields() {
    let q = decode_quote_bytes(QUOTE_V2).expect("v2 golden quote must decode");
    assert_eq!(q.symbol, "2330");
    assert_eq!(q.exchange, Exchange::Twse);
    assert_eq!(q.quote_ts_us, 1_700_000_000_000_000);
    assert_eq!(q.bids.len(), 2);
    assert_eq!(q.asks.len(), 1);
    assert_eq!(q.last_price, 58050);
    assert_eq!(q.last_volume, 10);
    assert_eq!(q.source, 0);
    assert_eq!(q.seq, 12345);
    assert_eq!(q.epoch, 7);
    assert_eq!(q.recv_ts_us, 1_700_000_000_000_042);
    assert_eq!(q.board, QuoteBoard::RoundLot);
    assert_eq!(q.session, Session::Day);
    assert_eq!(q.trading_date, 20_260_704);
    assert!(!q.simtrade);
    assert_eq!(q.underlying_price, 0);
}

#[test]
fn golden_trade_decodes_known_fields() {
    match decode_feed_event(TRADE).expect("golden trade must decode") {
        FeedEvent::Trade(t) => {
            assert_eq!(t.symbol, "2330");
            assert_eq!(t.exchange, Exchange::Twse);
            assert_eq!(t.source, 0);
            assert_eq!(t.seq, 12346);
            assert_eq!(t.epoch, 7);
            assert_eq!(t.trade_ts_us, 1_700_000_000_000_100);
            assert_eq!(t.recv_ts_us, 1_700_000_000_000_142);
            assert_eq!(t.price_mantissa, 58050);
            assert_eq!(t.price_scale, 2);
            assert_eq!(t.volume, 3);
            assert!(!t.is_trial);
            assert_eq!(t.session, Session::Day);
            assert_eq!(t.trading_date, 20_260_704);
        }
        other => panic!("expected a trade, got {other:?}"),
    }
}
