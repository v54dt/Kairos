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
use crate::ipc::idle_backoff;
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

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicU64;

    use crate::encode::{encode_quote, encode_subscribe};
    use crate::model::{Exchange, PriceLevel, Quote, QuoteBoard, Session};

    fn quote(source: u16, seq: u64, last: i64) -> Quote {
        Quote {
            symbol: "2330".to_owned(),
            exchange: Exchange::Twse,
            quote_ts_us: 0,
            bids: vec![PriceLevel {
                price_mantissa: last - 50,
                price_scale: 2,
                volume: 1,
            }],
            asks: vec![PriceLevel {
                price_mantissa: last + 50,
                price_scale: 2,
                volume: 1,
            }],
            last_price: last,
            last_scale: 2,
            last_volume: 1,
            is_trial: false,
            source,
            seq,
            epoch: 1,
            recv_ts_us: 0,
            board: QuoteBoard::RoundLot,
            session: Session::Unknown,
            trading_date: 0,
            simtrade: false,
            underlying_price: 0,
        }
    }

    fn deps(selector: Arc<Selector>) -> (PollDeps, broadcast::Receiver<FeedEvent>) {
        let (tx, rx) = broadcast::channel::<FeedEvent>(16);
        let d = PollDeps {
            book: Arc::new(RwLock::new(Book::new())),
            tx,
            metrics: Arc::new(Metrics::default()),
            selector,
        };
        (d, rx)
    }

    fn count(c: &AtomicU64) -> u64 {
        c.load(Ordering::Relaxed)
    }

    #[test]
    fn ordering_drop_increments_metric_and_does_not_broadcast() {
        let (d, mut rx) = deps(Arc::new(Selector::new(vec![0], 0, 0)));
        assert_eq!(
            handle_event(&d, &encode_quote(&quote(0, 5, 58_000)), 0),
            Outcome::Broadcast
        );
        assert_eq!(
            rx.try_recv().unwrap(),
            FeedEvent::Quote(quote(0, 5, 58_000))
        );
        // An older seq within the same (source, epoch) is dropped by the book guard.
        assert_eq!(
            handle_event(&d, &encode_quote(&quote(0, 4, 57_000)), 0),
            Outcome::Dropped
        );
        assert_eq!(count(&d.metrics.ordering_drops), 1);
        assert!(rx.try_recv().is_err());
    }

    #[test]
    fn inactive_source_notes_liveness_but_does_not_broadcast() {
        let selector = Arc::new(Selector::new(vec![0, 1], 50_000, 200_000));
        let (d, mut rx) = deps(selector.clone());
        // Source 1 is not the active source, so the tick is admitted to the book but
        // gated out of the broadcast — while its liveness is still noted.
        assert_eq!(
            handle_event(&d, &encode_quote(&quote(1, 1, 59_000)), 42_000),
            Outcome::AdmittedNotServing
        );
        assert!(rx.try_recv().is_err());
        assert_eq!(
            d.book.read().unwrap().get(1, "2330").unwrap().last_price,
            59_000
        );
        // The note landed: source 1 is now the freshest and eval fails over to it.
        assert!(selector.eval(60_000).is_some());
        assert_eq!(selector.active_source(), 1);
    }

    #[test]
    fn active_source_quote_broadcasts_and_updates_the_book() {
        let (d, mut rx) = deps(Arc::new(Selector::new(vec![0, 1], 50_000, 200_000)));
        assert_eq!(
            handle_event(&d, &encode_quote(&quote(0, 1, 58_000)), 0),
            Outcome::Broadcast
        );
        assert_eq!(
            rx.try_recv().unwrap(),
            FeedEvent::Quote(quote(0, 1, 58_000))
        );
        assert_eq!(
            d.book.read().unwrap().get(0, "2330").unwrap().last_price,
            58_000
        );
        assert_eq!(count(&d.metrics.quotes_decoded), 1);
    }

    #[test]
    fn control_and_garbage_variants_counted_not_broadcast() {
        let (d, mut rx) = deps(Arc::new(Selector::new(vec![0], 0, 0)));
        // A well-formed but unrouted variant (a control/subscribe frame).
        assert_eq!(
            handle_event(&d, &encode_subscribe(&["2330"]), 0),
            Outcome::UnknownVariant
        );
        assert_eq!(count(&d.metrics.unknown_variants), 1);
        // Malformed bytes are a genuine decode failure.
        assert_eq!(
            handle_event(&d, &[0x00, 0x01, 0x02], 0),
            Outcome::DecodeError
        );
        assert_eq!(count(&d.metrics.decode_errors), 1);
        assert!(rx.try_recv().is_err());
    }

    #[test]
    fn single_source_serves_without_reading_the_clock() {
        // handle_event never reads a clock (now_us is passed by value); run() supplies
        // 0 for a single-source feed, and the inert selector notes nothing and serves
        // everything — so this path is byte-identical to the single-feed pipeline.
        let selector = Arc::new(Selector::new(vec![0], 0, 0));
        assert!(!selector.is_multi());
        let (d, mut rx) = deps(selector.clone());
        assert_eq!(
            handle_event(&d, &encode_quote(&quote(7, 1, 58_000)), 0),
            Outcome::Broadcast
        );
        assert_eq!(
            rx.try_recv().unwrap(),
            FeedEvent::Quote(quote(7, 1, 58_000))
        );
        assert!(selector.serves(7));
    }
}
