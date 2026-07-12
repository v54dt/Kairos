use mimalloc::MiMalloc;

#[global_allocator]
static GLOBAL: MiMalloc = MiMalloc;

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex, RwLock};
use std::thread;
use std::time::Duration;

use kairos_core::book::Book;
use kairos_core::decode::FeedEvent;
use kairos_core::encode::encode_subscribe;
use kairos_core::failover::Selector;
use kairos_core::ipc::aeron::AeronPub;
use kairos_core::metrics::Metrics;
use kairos_core::poll::{PollDeps, run as run_poll};
use kairos_core::streams::StreamTable;
use kairos_core::subreg::SubRegistry;
use kairos_core::uds::path::quote_socket_path;
use kairos_core::uds::server::{ServerHandles, run_server};
use tokio::signal::unix::{SignalKind, signal};
use tokio::sync::{broadcast, watch};

/// Re-publish the desired set at least this often (also the upstream heartbeat
/// that recovers a sidecar restart/reconnect).
const CONTROL_HEARTBEAT: Duration = Duration::from_secs(10);

/// How often the failover selector re-evaluates the active source (multi-source only).
const FAILOVER_EVAL_INTERVAL: Duration = Duration::from_millis(250);

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

    let table = StreamTable::from_env().unwrap_or_else(|e| {
        eprintln!("kairos-core: FATAL invalid KAIROS_STREAMS: {e:?}");
        std::process::exit(1);
    });

    let priority = table
        .source_priority(std::env::var("KAIROS_SOURCE_PRIORITY").ok().as_deref())
        .unwrap_or_else(|e| {
            eprintln!("kairos-core: FATAL invalid KAIROS_SOURCE_PRIORITY: {e:?}");
            std::process::exit(1);
        });
    let selector = Arc::new(Selector::from_env(priority.clone()));
    if selector.is_multi() {
        eprintln!("kairos-core: multi-source failover active, priority {priority:?}");
        let eval_selector = selector.clone();
        let eval_metrics = metrics.clone();
        let eval_shutdown = shutting_down.clone();
        thread::spawn(move || failover_eval_loop(eval_selector, eval_metrics, eval_shutdown));
    }

    for entry in table.quotes() {
        let stream_id = entry.stream_id;
        let deps = PollDeps {
            book: book.clone(),
            tx: tx.clone(),
            metrics: metrics.clone(),
            selector: selector.clone(),
        };
        let poll_shutdown = shutting_down.clone();
        thread::spawn(move || run_poll(stream_id, deps, poll_shutdown));
    }

    let control_stream_id = table.control_stream_id();
    let control_registry = registry.clone();
    thread::spawn(move || control_publish_loop(control_stream_id, control_registry, change_rx));

    let socket_path = quote_socket_path();
    eprintln!("kairos-core: UDS quote server on {socket_path}");
    let handles = ServerHandles {
        book,
        quotes: tx,
        registry,
        change_tx,
        metrics,
        selector,
    };
    run_server(&socket_path, handles, shutdown_rx).await?;
    Ok(())
}

/// Periodically recompute the active source and loudly log every switch.
fn failover_eval_loop(
    selector: Arc<Selector>,
    metrics: Arc<Metrics>,
    shutting_down: Arc<AtomicBool>,
) {
    let primary = selector.active_source();
    loop {
        if shutting_down.load(Ordering::SeqCst) {
            return;
        }
        thread::sleep(FAILOVER_EVAL_INTERVAL);
        if let Some(sw) = selector.eval(selector.now_us()) {
            Metrics::inc(&metrics.failover_switches);
            let kind = if sw.to == primary {
                "FAILBACK"
            } else {
                "FAILOVER"
            };
            eprintln!(
                "kairos-core: {kind} active quote source {} -> {} (primary {primary})",
                sw.from, sw.to
            );
        }
    }
}

/// Publishes the desired subscription set to the sidecar on the control stream:
/// on every change, plus a periodic heartbeat. Until the sidecar wires stream
/// 1002 (Phase H2) there is no subscriber, so offers fail harmlessly.
fn control_publish_loop(
    control_stream_id: i32,
    registry: Arc<Mutex<SubRegistry>>,
    change_rx: mpsc::Receiver<()>,
) {
    let publisher = match AeronPub::connect(None, control_stream_id) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("kairos-core: FATAL control aeron connect failed: {e:?}");
            std::process::exit(1);
        }
    };
    eprintln!("kairos-core: subscription control publisher on stream {control_stream_id}");
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
