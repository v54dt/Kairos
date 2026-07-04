//! Cross-language wire-format regression test (Track C5).
//!
//! Decodes the shared golden fixture `schema/testdata/quote_golden_envelope.bin`
//! and asserts every field. The same fixture is asserted, byte-for-byte, by the
//! C++ encoder test `sidecar/concords/tests/test_golden.cpp`. If the Rust decoder
//! drifts from the frozen capnp wire format (schema reorder, field-mapping change,
//! serialization change), this test fails in CI.
//!
//! Known Quote encoded in the fixture (keep in sync with test_golden.cpp):
//!   symbol      = "2330"
//!   exchange    = Twse
//!   quote_ts_us = 1_700_000_000_000_000
//!   bids        = [{58000, 2, 100}, {57950, 2, 50}]
//!   asks        = [{58100, 2, 80}]
//!   last_price  = 58050
//!   last_scale  = 2
//!   last_volume = 10
//!   is_trial    = false

use kairos_core::decode::decode_quote_bytes;
use kairos_core::model::Exchange;

const GOLDEN: &[u8] = include_bytes!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../schema/testdata/quote_golden_envelope.bin"
));

#[test]
fn golden_quote_decodes_to_known_fields() {
    let q = decode_quote_bytes(GOLDEN).expect("golden fixture must decode");

    assert_eq!(q.symbol, "2330");
    assert_eq!(q.exchange, Exchange::Twse);
    assert_eq!(q.quote_ts_us, 1_700_000_000_000_000);

    assert_eq!(q.bids.len(), 2);
    assert_eq!(q.bids[0].price_mantissa, 58000);
    assert_eq!(q.bids[0].price_scale, 2);
    assert_eq!(q.bids[0].volume, 100);
    assert_eq!(q.bids[1].price_mantissa, 57950);
    assert_eq!(q.bids[1].price_scale, 2);
    assert_eq!(q.bids[1].volume, 50);

    assert_eq!(q.asks.len(), 1);
    assert_eq!(q.asks[0].price_mantissa, 58100);
    assert_eq!(q.asks[0].price_scale, 2);
    assert_eq!(q.asks[0].volume, 80);

    assert_eq!(q.last_price, 58050);
    assert_eq!(q.last_scale, 2);
    assert_eq!(q.last_volume, 10);
    assert!(!q.is_trial);
}
