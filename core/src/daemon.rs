//! Shared skeleton for the standalone daemons (recordd/replayd/sim): a ctrlc-driven
//! stop flag and an interruptible periodic stats loop, so each bin keeps only its
//! own render logic.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::{Duration, Instant};

/// Install a SIGINT/SIGTERM handler that flips a shared stop flag. Call once, before
/// spawning any worker, so an interrupt during bring-up unwinds cleanly.
pub fn install_stop_flag() -> anyhow::Result<Arc<AtomicBool>> {
    let stop = Arc::new(AtomicBool::new(false));
    ctrlc::set_handler({
        let stop = stop.clone();
        move || stop.store(true, Ordering::SeqCst)
    })?;
    Ok(stop)
}

/// Every 10s (interruptible in 200ms slices) print one `render()` line to stderr,
/// until `stop` is set. A final line is emitted on the cycle the stop is observed.
pub fn stats_loop<F: Fn() -> String>(stop: &AtomicBool, render: F) {
    while !stop.load(Ordering::Relaxed) {
        let until = Instant::now() + Duration::from_secs(10);
        while Instant::now() < until && !stop.load(Ordering::Relaxed) {
            thread::sleep(Duration::from_millis(200));
        }
        eprintln!("{}", render());
    }
}
