//! End-to-end replay test on a real (embedded) Aeron driver. Writes a KQR fixture
//! with the record writer, replays it full-speed through KqrSource + Pacer + an
//! AeronPub into the driver, subscribes, and asserts the received payloads are
//! byte-for-byte identical to what was recorded (and the exact count).
//!
//! Ignored by default (needs an embedded media driver): run with
//!   cargo test --release --test replay_roundtrip -- --ignored

use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use kairos_core::ipc::aeron::{AeronPub, AeronSub};
use kairos_core::record::{FileHeader, RecordWriter};
use kairos_core::replay::{
    KqrSource, OfferOutcome, Pace, Pacer, ReplayStats, SystemClock, drive_replay,
};
use rusteron_media_driver::{AeronDriver, AeronDriverContext, IntoCString};

const STREAM_ID: i32 = 1001;
const AERON_DIR: &str = "/dev/shm/aeron-kairos-replaytest";
const N: usize = 200;

fn frag(i: usize) -> Vec<u8> {
    (0..(8 + i % 40)).map(|b| (b + i) as u8).collect()
}

fn write_fixture() -> PathBuf {
    let path = std::env::temp_dir().join(format!("kairos-replaytest-{}.kqr", std::process::id()));
    let f = std::fs::File::create(&path).unwrap();
    let mut w = RecordWriter::create(f, &FileHeader::new(STREAM_ID as u32, 0)).unwrap();
    for i in 0..N {
        // Strictly increasing receive timestamps (10µs apart).
        w.append(i as i64 * 10, &frag(i)).unwrap();
    }
    w.flush().unwrap();
    path
}

#[test]
#[ignore = "needs an embedded Aeron media driver; run with --ignored"]
fn replayed_bytes_match_recorded() {
    let fixture = write_fixture();

    let driver_ctx = AeronDriverContext::new().unwrap();
    driver_ctx.set_dir_delete_on_start(true).unwrap();
    driver_ctx.set_dir_delete_on_shutdown(true).unwrap();
    driver_ctx.set_dir(&AERON_DIR.into_c_string()).unwrap();
    let (drv_stop, drv_handle) = AeronDriver::launch_embedded(driver_ctx.clone(), false);
    std::thread::sleep(Duration::from_millis(300));

    let subscriber = AeronSub::connect(Some(AERON_DIR), STREAM_ID).unwrap();

    // Replay runs on its own thread (Aeron handles are not Send, so it connects its
    // own publication there).
    let offered = Arc::new(AtomicBool::new(false));
    let replay = std::thread::spawn({
        let fixture = fixture.clone();
        let offered = offered.clone();
        move || {
            let publisher = AeronPub::connect(Some(AERON_DIR), STREAM_ID).unwrap();
            std::thread::sleep(Duration::from_millis(300)); // let the subscription connect
            let source = KqrSource::open_files(&[fixture]).unwrap();
            let stats = ReplayStats::new(&[STREAM_ID as u32]);
            let stop = AtomicBool::new(false);
            let pacer = Pacer::new(source, SystemClock::new(None), Pace::Full);
            drive_replay(pacer, &stop, &stats, |rec| {
                while publisher.offer(&rec.payload).is_err() {
                    std::thread::sleep(Duration::from_millis(1));
                }
                OfferOutcome::Offered
            });
            offered.store(true, Ordering::SeqCst);
            stats.offered.load(Ordering::Relaxed)
        }
    });

    let mut got: Vec<Vec<u8>> = Vec::new();
    let deadline = Instant::now() + Duration::from_secs(15);
    while got.len() < N && Instant::now() < deadline {
        subscriber.poll(|data| got.push(data.to_vec()), 64).unwrap();
        std::thread::sleep(Duration::from_millis(2));
    }

    let replay_offered = replay.join().unwrap();
    drv_stop.store(true, Ordering::SeqCst);
    let _ = drv_handle.join();
    let _ = std::fs::remove_file(&fixture);

    assert!(offered.load(Ordering::SeqCst), "replay did not finish");
    assert_eq!(replay_offered, N as u64, "not all fragments offered");
    assert_eq!(got.len(), N, "not all fragments received");
    let want: Vec<Vec<u8>> = (0..N).map(frag).collect();
    assert_eq!(got, want, "replayed payloads differ from what was recorded");
}
