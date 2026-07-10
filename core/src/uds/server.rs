use std::collections::HashSet;
use std::sync::{Arc, Mutex, RwLock};

use tokio::net::UnixListener;
use tokio::net::UnixStream;
use tokio::net::unix::{OwnedReadHalf, OwnedWriteHalf};
use tokio::sync::{broadcast, mpsc, watch};
use tokio::task::JoinSet;

use crate::book::Book;
use crate::decode::{FeedEvent, Message, decode_message_bytes};
use crate::encode::{encode_error, encode_quote, encode_sub_ack, encode_trade};
use crate::metrics::Metrics;
use crate::model::Quote;
use crate::subreg::SubRegistry;
use crate::uds::frame::{read_frame, write_frame};

const SNAPSHOT_CHANNEL: usize = 256;

pub async fn run_server(
    socket_path: &str,
    book: Arc<RwLock<Book>>,
    quotes: broadcast::Sender<FeedEvent>,
    registry: Arc<Mutex<SubRegistry>>,
    change_tx: std::sync::mpsc::Sender<()>,
    metrics: Arc<Metrics>,
    mut shutdown: watch::Receiver<bool>,
) -> std::io::Result<()> {
    let _ = std::fs::remove_file(socket_path);
    let listener = UnixListener::bind(socket_path)?;
    let mut clients = JoinSet::new();
    loop {
        tokio::select! {
            biased;
            // A shutdown signal (or a dropped sender) stops new intake immediately.
            _ = shutdown.changed() => break,
            // Reap completed clients so the JoinSet does not retain finished tasks.
            Some(_) = clients.join_next() => {}
            accepted = listener.accept() => {
                let (stream, _) = accepted?;
                clients.spawn(handle_client(
                    stream,
                    book.clone(),
                    quotes.clone(),
                    registry.clone(),
                    change_tx.clone(),
                    metrics.clone(),
                    shutdown.clone(),
                ));
            }
        }
    }
    // Stop accepting and let every client finish its in-flight frame, then EOF. A
    // client only breaks BETWEEN frames, so no length-prefixed frame is ever cut.
    while clients.join_next().await.is_some() {}
    let _ = std::fs::remove_file(socket_path);
    Ok(())
}

async fn handle_client(
    stream: UnixStream,
    book: Arc<RwLock<Book>>,
    quotes: broadcast::Sender<FeedEvent>,
    registry: Arc<Mutex<SubRegistry>>,
    change_tx: std::sync::mpsc::Sender<()>,
    metrics: Arc<Metrics>,
    shutdown: watch::Receiver<bool>,
) {
    metrics
        .clients
        .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let (read_half, write_half) = stream.into_split();
    let subs = Arc::new(Mutex::new(HashSet::<String>::new()));
    let (snap_tx, snap_rx) = mpsc::channel::<Vec<u8>>(SNAPSHOT_CHANNEL);
    let writer = tokio::spawn(writer_loop(
        write_half,
        quotes.subscribe(),
        subs.clone(),
        snap_rx,
        metrics.clone(),
        shutdown.clone(),
    ));
    reader_loop(
        read_half, book, subs, snap_tx, registry, change_tx, shutdown,
    )
    .await;
    // Await the writer instead of aborting it: it stops between frames, so the client
    // sees whole frames then a clean EOF when the write half drops here.
    let _ = writer.await;
    metrics
        .clients
        .fetch_sub(1, std::sync::atomic::Ordering::Relaxed);
}

async fn reader_loop(
    mut read_half: OwnedReadHalf,
    book: Arc<RwLock<Book>>,
    subs: Arc<Mutex<HashSet<String>>>,
    snap_tx: mpsc::Sender<Vec<u8>>,
    registry: Arc<Mutex<SubRegistry>>,
    change_tx: std::sync::mpsc::Sender<()>,
    mut shutdown: watch::Receiver<bool>,
) {
    loop {
        let frame = tokio::select! {
            biased;
            _ = shutdown.changed() => break,
            r = read_frame(&mut read_half) => match r {
                Ok(Some(frame)) => frame,
                _ => break,
            },
        };
        match decode_message_bytes(&frame) {
            Ok(Message::Subscribe(syms)) => {
                let mut changed = false;
                {
                    let mut s = subs.lock().unwrap();
                    let mut reg = registry.lock().unwrap();
                    for sym in &syms {
                        if s.insert(sym.clone()) {
                            changed |= reg.add(sym);
                        }
                    }
                }
                if changed {
                    let _ = change_tx.send(());
                }
                // Ack the subscription, then push the current snapshot per symbol.
                let _ = snap_tx.try_send(encode_sub_ack(&syms, true));
                let snapshots: Vec<Quote> = {
                    let b = book.read().unwrap();
                    syms.iter().filter_map(|sym| b.get(sym).cloned()).collect()
                };
                // try_send (not await): drop the frame if the channel is full rather
                // than stalling the reader on a slow consumer — the live broadcast
                // refreshes it on the symbol's next tick.
                for q in snapshots {
                    let _ = snap_tx.try_send(encode_quote(&q));
                }
            }
            Ok(Message::Unsubscribe(syms)) => {
                let mut changed = false;
                {
                    let mut s = subs.lock().unwrap();
                    let mut reg = registry.lock().unwrap();
                    for sym in &syms {
                        if s.remove(sym) {
                            changed |= reg.remove(sym);
                        }
                    }
                }
                if changed {
                    let _ = change_tx.send(());
                }
            }
            _ => {}
        }
    }
    // Client gone: release all of its refcounts so the desired set shrinks.
    let mut changed = false;
    {
        let mut s = subs.lock().unwrap();
        let mut reg = registry.lock().unwrap();
        for sym in s.drain() {
            changed |= reg.remove(&sym);
        }
    }
    if changed {
        let _ = change_tx.send(());
    }
}

async fn writer_loop(
    mut write_half: OwnedWriteHalf,
    mut quotes: broadcast::Receiver<FeedEvent>,
    subs: Arc<Mutex<HashSet<String>>>,
    mut snap_rx: mpsc::Receiver<Vec<u8>>,
    metrics: Arc<Metrics>,
    mut shutdown: watch::Receiver<bool>,
) {
    loop {
        // Once a data branch is chosen, its write_frame runs to completion before the
        // next select — the shutdown branch can only interrupt between frames.
        tokio::select! {
            biased;
            _ = shutdown.changed() => break,
            live = quotes.recv() => match live {
                Ok(event) => {
                    let want = subs.lock().unwrap().contains(event.symbol());
                    if want {
                        let frame = match &event {
                            FeedEvent::Quote(q) => encode_quote(q),
                            FeedEvent::Trade(t) => encode_trade(t),
                        };
                        if write_frame(&mut write_half, &frame).await.is_err() {
                            break;
                        }
                    }
                }
                Err(broadcast::error::RecvError::Lagged(n)) => {
                    Metrics::inc(&metrics.lagged);
                    let e = encode_error(&format!("lagged {n}"));
                    if write_frame(&mut write_half, &e).await.is_err() {
                        break;
                    }
                }
                Err(broadcast::error::RecvError::Closed) => break,
            },
            snap = snap_rx.recv() => match snap {
                Some(frame) => {
                    if write_frame(&mut write_half, &frame).await.is_err() {
                        break;
                    }
                }
                None => break,
            },
        }
    }
    // Flush any buffered bytes so the final whole frame reaches the client before EOF.
    let _ = tokio::io::AsyncWriteExt::shutdown(&mut write_half).await;
}
