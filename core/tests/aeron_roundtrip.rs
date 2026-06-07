use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

use kairos_core::book::Book;
use kairos_core::decode::decode_quote_bytes;
use kairos_core::encode::encode_quote;
use kairos_core::ipc::aeron::{AeronPub, AeronSub};
use kairos_core::model::{Exchange, PriceLevel, Quote};
use rusteron_media_driver::{AeronDriver, AeronDriverContext, IntoCString};

const STREAM_ID: i32 = 1001;
const AERON_DIR: &str = "/dev/shm/aeron-kairos-test";

fn sample_quote() -> Quote {
    Quote {
        symbol: "2330".to_owned(),
        exchange: Exchange::Twse,
        quote_ts_us: 1_700_000_000_000_000,
        bids: vec![PriceLevel {
            price_mantissa: 58000,
            price_scale: 2,
            volume: 100,
        }],
        asks: vec![PriceLevel {
            price_mantissa: 58100,
            price_scale: 2,
            volume: 80,
        }],
        last_price: 58050,
        last_scale: 2,
        last_volume: 10,
        is_trial: false,
    }
}

#[test]
#[ignore = "needs an embedded Aeron media driver; run with --ignored"]
fn aeron_quote_roundtrip() {
    let driver_ctx = AeronDriverContext::new().unwrap();
    driver_ctx.set_dir_delete_on_start(true).unwrap();
    driver_ctx.set_dir_delete_on_shutdown(true).unwrap();
    driver_ctx.set_dir(&AERON_DIR.into_c_string()).unwrap();
    let (stop, handle) = AeronDriver::launch_embedded(driver_ctx.clone(), false);
    std::thread::sleep(Duration::from_millis(300));

    let publisher = AeronPub::connect(Some(AERON_DIR), STREAM_ID).unwrap();
    let subscriber = AeronSub::connect(Some(AERON_DIR), STREAM_ID).unwrap();

    let bytes = encode_quote(&sample_quote());
    let mut book = Book::new();
    let deadline = Instant::now() + Duration::from_secs(5);
    while book.is_empty() && Instant::now() < deadline {
        let _ = publisher.offer(&bytes);
        subscriber
            .poll(
                |data| {
                    if let Ok(q) = decode_quote_bytes(data) {
                        book.update(q);
                    }
                },
                10,
            )
            .unwrap();
        std::thread::sleep(Duration::from_millis(20));
    }

    stop.store(true, Ordering::SeqCst);
    let _ = handle.join();

    assert_eq!(
        book.get("2330").map(|q| q.last_price),
        Some(58050),
        "quote not received over aeron"
    );
}
