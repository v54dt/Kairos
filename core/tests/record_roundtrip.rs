//! End-to-end recorder test on a real (embedded) Aeron driver, driven by
//! synthetic fragments — no market feed or market hours needed. Publishes N
//! distinct fragments on stream 1001, records them, and asserts the KQR file
//! reads back byte-identical.
//!
//! Ignored by default (needs an embedded media driver): run with
//!   cargo test --release --test record_roundtrip -- --ignored

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use kairos_core::ipc::aeron::AeronPub;
use kairos_core::record::recorder::{Stats, recv_ts_now_us, run_stream, yyyymmdd_utc8};
use kairos_core::record::{RecordReader, record_path, valid_prefix_len};
use rusteron_media_driver::{AeronDriver, AeronDriverContext, IntoCString};

const STREAM_ID: i32 = 1001;
const AERON_DIR: &str = "/dev/shm/aeron-kairos-rectest";
const N: usize = 200;

fn frag(i: usize) -> Vec<u8> {
    // distinct length + content per message
    (0..(8 + i % 40)).map(|b| (b + i) as u8).collect()
}

#[test]
#[ignore = "needs an embedded Aeron media driver; run with --ignored"]
fn recorder_captures_fragments_byte_identical() {
    let driver_ctx = AeronDriverContext::new().unwrap();
    driver_ctx.set_dir_delete_on_start(true).unwrap();
    driver_ctx.set_dir_delete_on_shutdown(true).unwrap();
    driver_ctx.set_dir(&AERON_DIR.into_c_string()).unwrap();
    let (drv_stop, drv_handle) = AeronDriver::launch_embedded(driver_ctx.clone(), false);
    std::thread::sleep(Duration::from_millis(300));

    let out_dir = std::env::temp_dir().join(format!("kairos-rectest-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&out_dir);
    let day = yyyymmdd_utc8(recv_ts_now_us());

    let stop = Arc::new(AtomicBool::new(false));
    let stats = Arc::new(Stats::default());
    let rec = std::thread::spawn({
        let (out, stop, stats) = (out_dir.clone(), stop.clone(), stats.clone());
        move || run_stream(STREAM_ID, Some(AERON_DIR.to_owned()), out, stop, stats)
    });
    std::thread::sleep(Duration::from_millis(300)); // let the subscription connect

    let publisher = AeronPub::connect(Some(AERON_DIR), STREAM_ID).unwrap();
    for i in 0..N {
        let msg = frag(i);
        while publisher.offer(&msg).is_err() {
            std::thread::sleep(Duration::from_millis(1));
        }
    }

    let deadline = Instant::now() + Duration::from_secs(10);
    while stats.records.load(Ordering::Relaxed) < N as u64 && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(20));
    }
    stop.store(true, Ordering::SeqCst);
    let _ = rec.join();
    drv_stop.store(true, Ordering::SeqCst);
    let _ = drv_handle.join();

    assert_eq!(
        stats.records.load(Ordering::Relaxed),
        N as u64,
        "not all fragments recorded"
    );

    let path = record_path(&out_dir, STREAM_ID as u32, &day);
    let data = std::fs::read(&path).unwrap();
    assert_eq!(
        valid_prefix_len(&data).unwrap(),
        data.len(),
        "file has a torn tail"
    );
    let (hdr, r) = RecordReader::open(&data[..]).unwrap();
    assert_eq!(hdr.stream_id, STREAM_ID as u32);
    let got: Vec<Vec<u8>> = r.map(|x| x.unwrap().payload).collect();
    let want: Vec<Vec<u8>> = (0..N).map(frag).collect();
    assert_eq!(
        got, want,
        "recorded payloads differ from what was published"
    );

    let _ = std::fs::remove_dir_all(&out_dir);
}
