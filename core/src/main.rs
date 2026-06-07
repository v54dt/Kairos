use std::sync::mpsc;
use std::sync::{Arc, Mutex, RwLock};
use std::thread;
use std::time::Duration;

use kairos_core::book::Book;
use kairos_core::decode::decode_quote_bytes;
use kairos_core::encode::encode_subscribe;
use kairos_core::ipc::aeron::{AeronPub, AeronSub, CONTROL_STREAM_ID, DEFAULT_STREAM_ID};
use kairos_core::model::Quote;
use kairos_core::subreg::SubRegistry;
use kairos_core::uds::path::quote_socket_path;
use kairos_core::uds::server::run_server;
use tokio::sync::broadcast;

/// Re-publish the desired set at least this often (also the upstream heartbeat
/// that recovers a sidecar restart/reconnect).
const CONTROL_HEARTBEAT: Duration = Duration::from_secs(10);

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let book = Arc::new(RwLock::new(Book::new()));
    let (tx, _) = broadcast::channel::<Quote>(1024);
    let registry = Arc::new(Mutex::new(SubRegistry::new()));
    let (change_tx, change_rx) = mpsc::channel::<()>();

    let aeron_book = book.clone();
    let aeron_tx = tx.clone();
    thread::spawn(move || aeron_poll_loop(aeron_book, aeron_tx));

    let control_registry = registry.clone();
    thread::spawn(move || control_publish_loop(control_registry, change_rx));

    let socket_path = quote_socket_path();
    eprintln!("kairos-core: UDS quote server on {socket_path}");
    run_server(&socket_path, book, tx, registry, change_tx).await?;
    Ok(())
}

/// Publishes the desired subscription set to the sidecar on the control stream:
/// on every change, plus a periodic heartbeat. Until the sidecar wires stream
/// 1002 (Phase H2) there is no subscriber, so offers fail harmlessly.
fn control_publish_loop(registry: Arc<Mutex<SubRegistry>>, change_rx: mpsc::Receiver<()>) {
    let publisher = match AeronPub::connect(None, CONTROL_STREAM_ID) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("kairos-core: control aeron connect failed: {e:?}");
            return;
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

fn aeron_poll_loop(book: Arc<RwLock<Book>>, tx: broadcast::Sender<Quote>) {
    let sub = match AeronSub::connect(None, DEFAULT_STREAM_ID) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("kairos-core: aeron connect failed: {e:?}");
            return;
        }
    };
    loop {
        let _ = sub.poll(
            |data| {
                if let Ok(q) = decode_quote_bytes(data) {
                    book.write().unwrap().update(q.clone());
                    let _ = tx.send(q);
                }
            },
            64,
        );
    }
}
