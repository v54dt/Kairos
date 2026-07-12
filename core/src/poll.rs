//! The Aeron quote poll loop and its per-event gate. `handle_event` is the sole
//! domain logic on the hot decode path (decode -> latency observe -> selector
//! note -> book update/ordering guard -> serves() gate -> broadcast) and is the
//! real code the failover/serve tests exercise. `run` owns the connect,
//! fragment polling, watchdogs and idle backoff around it; the process exits it
//! takes are hard-failure restart signals for the supervisor.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, RwLock};
use std::time::{Duration, Instant};

use tokio::sync::broadcast;

use crate::book::{Admit, Book};
use crate::decode::{DecodeError, FeedEvent, decode_feed_event};
use crate::failover::Selector;
use crate::ipc::aeron::AeronSub;
use crate::metrics::Metrics;
use crate::watchdog::{
    DRIVER_DEAD_GRACE, DriverLivenessWatchdog, PollErrorWatchdog, driver_timeout_ms_from_env,
    max_poll_errors_from_env,
};

/// How often the poll loop probes the media driver's CnC heartbeat.
const LIVENESS_CHECK_INTERVAL: Duration = Duration::from_secs(1);

/// Shared handles the per-event gate and the poll loop operate on.
pub struct PollDeps {
    pub book: Arc<RwLock<Book>>,
    pub tx: broadcast::Sender<FeedEvent>,
    pub metrics: Arc<Metrics>,
    pub selector: Arc<Selector>,
}

/// What `handle_event` did with one decoded fragment. Broadcast means it was
/// sent to consumers; the other variants are all the non-serving/dropped/counted
/// outcomes and never touch `tx`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    Broadcast,
    AdmittedNotServing,
    Dropped,
    NotServing,
    UnknownVariant,
    DecodeError,
}

/// Decode one fragment and run the serve gate. `now_us` is the selector clock
/// read once per poll iteration and passed in, so a single-source run stays off
/// the clock entirely on the hot path.
pub fn handle_event(deps: &PollDeps, data: &[u8], now_us: u64) -> Outcome {
    match decode_feed_event(data) {
        Ok(FeedEvent::Quote(q)) => {
            Metrics::inc(&deps.metrics.quotes_decoded);
            deps.metrics
                .observe_latency(q.source, q.quote_ts_us, q.recv_ts_us);
            deps.selector.note(q.source, now_us);
            match deps.book.write().unwrap().update(q.clone()) {
                Admit::Admitted if deps.selector.serves(q.source) => {
                    let _ = deps.tx.send(FeedEvent::Quote(q));
                    Outcome::Broadcast
                }
                Admit::Admitted => Outcome::AdmittedNotServing,
                Admit::Dropped => {
                    Metrics::inc(&deps.metrics.ordering_drops);
                    Outcome::Dropped
                }
            }
        }
        Ok(FeedEvent::Trade(t)) => {
            deps.metrics
                .observe_latency(t.source, t.trade_ts_us, t.recv_ts_us);
            deps.selector.note(t.source, now_us);
            if deps.selector.serves(t.source) {
                let _ = deps.tx.send(FeedEvent::Trade(t));
                Outcome::Broadcast
            } else {
                Outcome::NotServing
            }
        }
        Err(DecodeError::UnknownVariant) => {
            Metrics::inc(&deps.metrics.unknown_variants);
            Outcome::UnknownVariant
        }
        Err(_) => {
            Metrics::inc(&deps.metrics.decode_errors);
            Outcome::DecodeError
        }
    }
}

/// Connect the stream and poll it until shutdown, driving `handle_event` per
/// fragment. The FATAL exits are the restart contract with the supervisor.
pub fn run(stream_id: i32, deps: PollDeps, shutting_down: Arc<AtomicBool>) {
    let sub = match AeronSub::connect(None, stream_id) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("kairos-core: FATAL aeron connect failed on stream {stream_id}: {e:?}");
            std::process::exit(1);
        }
    };
    let mut idle: u32 = 0;
    let mut last_err: Option<Instant> = None;
    let mut poll_wd = PollErrorWatchdog::new(max_poll_errors_from_env());
    let mut live_wd = DriverLivenessWatchdog::new(DRIVER_DEAD_GRACE);
    let driver_timeout_ms = driver_timeout_ms_from_env();
    let mut last_liveness = Instant::now();
    loop {
        if shutting_down.load(Ordering::SeqCst) {
            return;
        }
        if last_liveness.elapsed() >= LIVENESS_CHECK_INTERVAL {
            last_liveness = Instant::now();
            if live_wd.observe(sub.driver_active(driver_timeout_ms), Instant::now())
                && !shutting_down.load(Ordering::SeqCst)
            {
                eprintln!(
                    "kairos-core: FATAL media driver inactive (no CnC heartbeat for >{driver_timeout_ms}ms); exiting for restart"
                );
                std::process::exit(1);
            }
        }
        let now_us = if deps.selector.is_multi() {
            deps.selector.now_us()
        } else {
            0
        };
        match sub.poll(
            |data| {
                handle_event(&deps, data, now_us);
            },
            64,
        ) {
            Ok(n) if n > 0 => {
                idle = 0;
                poll_wd.on_ok();
            }
            Ok(_) => {
                poll_wd.on_ok();
                idle_backoff(&mut idle);
            }
            Err(e) => {
                if poll_wd.on_err() && !shutting_down.load(Ordering::SeqCst) {
                    eprintln!(
                        "kairos-core: FATAL aeron poll errored {} times consecutively ({e:?}); exiting for restart",
                        poll_wd.threshold()
                    );
                    std::process::exit(1);
                }
                if last_err.is_none_or(|t| t.elapsed() >= Duration::from_secs(5)) {
                    eprintln!("kairos-core: aeron poll error: {e:?}");
                    last_err = Some(Instant::now());
                }
                idle_backoff(&mut idle);
            }
        }
    }
}

// Spin → yield → park so an idle feed doesn't burn a core.
fn idle_backoff(idle: &mut u32) {
    *idle = idle.saturating_add(1);
    if *idle < 10 {
        std::hint::spin_loop();
    } else if *idle < 20 {
        std::thread::yield_now();
    } else {
        std::thread::sleep(Duration::from_micros(50));
    }
}
