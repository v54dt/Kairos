//! End-to-end replay of a generated tape over a real (embedded) Aeron driver.
//! Generates a trend-day tape with kairos-tapegen's library, replays it full-speed
//! through KqrSource + Pacer + an AeronPub into the driver, subscribes, decodes the
//! received envelopes, and asserts BOTH Quote and Trade events flow with in-window
//! timestamps — i.e. the tape carries the Trade prints the fill model needs.
//!
//! Ignored by default (needs an embedded media driver): run with
//!   cargo test --release --test tapegen_sim_e2e -- --ignored
//!
//! The heavier fills proof (replay through a trader and assert journal fills) is a
//! manual run:
//!   core/target/release/kairos-sim replay <tape> --symbols 2330 \
//!     --bin-dir core/target/release --hubd exec/scenario/build/kairos_sim_hubd

use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use kairos_core::decode::{FeedEvent, decode_feed_event};
use kairos_core::ipc::aeron::{AeronPub, AeronSub};
use kairos_core::record::{FileHeader, RecordWriter};
use kairos_core::replay::{
    KqrSource, OfferOutcome, Pace, Pacer, ReplayStats, SystemClock, drive_replay,
};
use kairos_core::tapegen::{GenParams, STREAM_ID, in_session_window, write_tape};
use rusteron_media_driver::{AeronDriver, AeronDriverContext, IntoCString};

const AERON_DIR: &str = "/dev/shm/aeron-kairos-tapegen-e2e";

fn write_trend_tape() -> (PathBuf, usize) {
    let params = GenParams::fixture_trend_day();
    let path = std::env::temp_dir().join(format!("kairos-tapegen-e2e-{}.kqr", std::process::id()));
    let f = std::fs::File::create(&path).unwrap();
    let mut w =
        RecordWriter::create(f, &FileHeader::new(STREAM_ID, params.session_open_us())).unwrap();
    let stats = write_tape(&mut w, &params).unwrap();
    w.flush().unwrap();
    (path, stats.records as usize)
}

#[test]
#[ignore = "needs an embedded Aeron media driver; run with --ignored"]
fn generated_tape_flows_quotes_and_trades() {
    let (fixture, n) = write_trend_tape();

    let driver_ctx = AeronDriverContext::new().unwrap();
    driver_ctx.set_dir_delete_on_start(true).unwrap();
    driver_ctx.set_dir_delete_on_shutdown(true).unwrap();
    driver_ctx.set_dir(&AERON_DIR.into_c_string()).unwrap();
    let (drv_stop, drv_handle) = AeronDriver::launch_embedded(driver_ctx.clone(), false);
    std::thread::sleep(Duration::from_millis(300));

    let subscriber = AeronSub::connect(Some(AERON_DIR), STREAM_ID as i32).unwrap();

    let offered = Arc::new(AtomicBool::new(false));
    let replay = std::thread::spawn({
        let fixture = fixture.clone();
        let offered = offered.clone();
        move || {
            let publisher = AeronPub::connect(Some(AERON_DIR), STREAM_ID as i32).unwrap();
            std::thread::sleep(Duration::from_millis(300));
            let source = KqrSource::open_files(&[fixture]).unwrap();
            let stats = ReplayStats::new(&[STREAM_ID]);
            let stop = AtomicBool::new(false);
            let pacer = Pacer::new(source, SystemClock::new(None), Pace::Full);
            drive_replay(pacer, &stop, &stats, |rec| {
                while publisher.offer(&rec.payload).is_err() {
                    std::thread::sleep(Duration::from_millis(1));
                }
                OfferOutcome::Offered
            });
            offered.store(true, Ordering::SeqCst);
        }
    });

    let mut got: Vec<Vec<u8>> = Vec::new();
    let deadline = Instant::now() + Duration::from_secs(15);
    while got.len() < n && Instant::now() < deadline {
        subscriber.poll(|data| got.push(data.to_vec()), 64).unwrap();
        std::thread::sleep(Duration::from_millis(2));
    }

    let _ = replay.join();
    drv_stop.store(true, Ordering::SeqCst);
    let _ = drv_handle.join();
    let _ = std::fs::remove_file(&fixture);

    assert!(offered.load(Ordering::SeqCst), "replay did not finish");
    assert_eq!(got.len(), n, "not all records received");

    let mut quotes = 0usize;
    let mut trades = 0usize;
    let mut below_reference = false;
    for bytes in &got {
        match decode_feed_event(bytes).unwrap() {
            FeedEvent::Quote(q) => {
                assert!(in_session_window(q.recv_ts_us));
                quotes += 1;
            }
            FeedEvent::Trade(t) => {
                assert!(in_session_window(t.recv_ts_us));
                if t.price_mantissa < 57_950 {
                    below_reference = true;
                }
                trades += 1;
            }
        }
    }
    assert!(quotes > 0, "no quotes flowed");
    assert!(trades > 0, "no trades flowed (tape cannot produce fills)");
    assert!(
        below_reference,
        "no trade printed below the join reference (fill model would not cross)"
    );
}
