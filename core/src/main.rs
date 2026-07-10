use mimalloc::MiMalloc;

#[global_allocator]
static GLOBAL: MiMalloc = MiMalloc;

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex, RwLock};
use std::thread;
use std::time::{Duration, Instant};

use kairos_core::book::Book;
use kairos_core::decode::{DecodeError, FeedEvent, decode_feed_event};
use kairos_core::encode::encode_subscribe;
use kairos_core::ipc::aeron::{AeronPub, AeronSub, CONTROL_STREAM_ID, DEFAULT_STREAM_ID};
use kairos_core::metrics::Metrics;
use kairos_core::subreg::SubRegistry;
use kairos_core::uds::path::quote_socket_path;
use kairos_core::uds::server::run_server;
use kairos_core::watchdog::{
    DRIVER_DEAD_GRACE, DriverLivenessWatchdog, PollErrorWatchdog, driver_timeout_ms_from_env,
    max_poll_errors_from_env,
};
use tokio::signal::unix::{SignalKind, signal};
use tokio::sync::{broadcast, watch};

/// How often the poll loop probes the media driver's CnC heartbeat.
const LIVENESS_CHECK_INTERVAL: Duration = Duration::from_secs(1);

/// Re-publish the desired set at least this often (also the upstream heartbeat
/// that recovers a sidecar restart/reconnect).
const CONTROL_HEARTBEAT: Duration = Duration::from_secs(10);

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let book = Arc::new(RwLock::new(Book::new()));
    let (tx, _) = broadcast::channel::<FeedEvent>(1024);
    let registry = Arc::new(Mutex::new(SubRegistry::new()));
    let (change_tx, change_rx) = mpsc::channel::<()>();
    let metrics = Arc::new(Metrics::default());
    metrics.clone().spawn_logger();

    // On SIGTERM/SIGINT: flip the async watch (server stops accepting and drains
    // in-flight frames) and the sync flag the poll thread reads (so a driver-timeout
    // race during teardown cannot turn a clean stop into exit 1).
    let (shutdown_tx, shutdown_rx) = watch::channel(false);
    let shutting_down = Arc::new(AtomicBool::new(false));
    let signal_flag = shutting_down.clone();
    tokio::spawn(async move {
        let mut sigterm = signal(SignalKind::terminate()).expect("install SIGTERM handler");
        let mut sigint = signal(SignalKind::interrupt()).expect("install SIGINT handler");
        tokio::select! {
            _ = sigterm.recv() => {}
            _ = sigint.recv() => {}
        }
        eprintln!("kairos-core: shutdown signal received; draining and exiting");
        signal_flag.store(true, Ordering::SeqCst);
        let _ = shutdown_tx.send(true);
    });

    let aeron_book = book.clone();
    let aeron_tx = tx.clone();
    let aeron_metrics = metrics.clone();
    let aeron_shutdown = shutting_down.clone();
    thread::spawn(move || aeron_poll_loop(aeron_book, aeron_tx, aeron_metrics, aeron_shutdown));

    let control_registry = registry.clone();
    thread::spawn(move || control_publish_loop(control_registry, change_rx));

    let socket_path = quote_socket_path();
    eprintln!("kairos-core: UDS quote server on {socket_path}");
    run_server(
        &socket_path,
        book,
        tx,
        registry,
        change_tx,
        metrics,
        shutdown_rx,
    )
    .await?;
    Ok(())
}

/// Publishes the desired subscription set to the sidecar on the control stream:
/// on every change, plus a periodic heartbeat. Until the sidecar wires stream
/// 1002 (Phase H2) there is no subscriber, so offers fail harmlessly.
fn control_publish_loop(registry: Arc<Mutex<SubRegistry>>, change_rx: mpsc::Receiver<()>) {
    let publisher = match AeronPub::connect(None, CONTROL_STREAM_ID) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("kairos-core: FATAL control aeron connect failed: {e:?}");
            std::process::exit(1);
        }
    };
    eprintln!("kairos-core: subscription control publisher on stream {CONTROL_STREAM_ID}");
    loop {
        match change_rx.recv_timeout(CONTROL_HEARTBEAT) {
            Ok(()) => while change_rx.try_recv().is_ok() {}, // coalesce a burst of changes
            Err(mpsc::RecvTimeoutError::Timeout) => {}       // heartbeat re-publish
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
        }
        let desired = registry.lock().unwrap().desired();
        let refs: Vec<&str> = desired.iter().map(String::as_str).collect();
        let _ = publisher.offer(&encode_subscribe(&refs));
    }
}

fn aeron_poll_loop(
    book: Arc<RwLock<Book>>,
    tx: broadcast::Sender<FeedEvent>,
    metrics: Arc<Metrics>,
    shutting_down: Arc<AtomicBool>,
) {
    let sub = match AeronSub::connect(None, DEFAULT_STREAM_ID) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("kairos-core: FATAL aeron connect failed: {e:?}");
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
        match sub.poll(
            |data| match decode_feed_event(data) {
                Ok(FeedEvent::Quote(q)) => {
                    Metrics::inc(&metrics.quotes_decoded);
                    book.write().unwrap().update(q.clone());
                    let _ = tx.send(FeedEvent::Quote(q));
                }
                Ok(FeedEvent::Trade(t)) => {
                    let _ = tx.send(FeedEvent::Trade(t));
                }
                Err(DecodeError::UnknownVariant) => Metrics::inc(&metrics.unknown_variants),
                Err(_) => Metrics::inc(&metrics.decode_errors),
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
