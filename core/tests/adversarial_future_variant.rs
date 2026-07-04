//! Regression: a FUTURE Envelope union variant (a discriminant this build's
//! schema does not know, e.g. a `@7` added by a newer sidecar in a recording an
//! older core reads) is classified as DecodeError::UnknownVariant — matching
//! that variant's own doc (decode.rs) which covers "a future variant an old
//! build cannot route". It must NOT surface as a capnp/NotInSchema error, which
//! main.rs's `Err(_)` arm would count as `decode_errors` (genuine corruption)
//! instead of the `unknown_variants` bucket.

use capnp::message::Builder;
use capnp::serialize;
use kairos_core::decode::{DecodeError, decode_feed_event};
use kairos_core::kairos_capnp;

fn forge_future_variant() -> Vec<u8> {
    // A heartbeat envelope is 32 bytes with the union discriminant (value 5) in
    // the struct data section at byte offset 16. Overwrite it with 7 (no such
    // variant exists in this schema) to simulate a newer build's Envelope.
    let mut hb = Vec::new();
    let mut m = Builder::new_default();
    m.init_root::<kairos_capnp::envelope::Builder>()
        .set_heartbeat(());
    serialize::write_message(&mut hb, &m).unwrap();
    assert_eq!(
        u16::from_le_bytes([hb[16], hb[17]]),
        5,
        "discriminant moved"
    );
    hb[16..18].copy_from_slice(&7u16.to_le_bytes());
    hb
}

#[test]
fn future_variant_is_counted_as_unknown_variant() {
    let bytes = forge_future_variant();
    let err = decode_feed_event(&bytes).expect_err("forged @7 must not decode as a feed event");
    match err {
        DecodeError::UnknownVariant => {}
        other => panic!("expected UnknownVariant for a future variant, got {other:?}"),
    }
}
