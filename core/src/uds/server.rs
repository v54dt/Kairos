use std::collections::HashSet;
use std::sync::{Arc, Mutex, RwLock};

use tokio::net::UnixListener;
use tokio::net::UnixStream;
use tokio::net::unix::{OwnedReadHalf, OwnedWriteHalf};
use tokio::sync::{broadcast, mpsc};

use crate::book::Book;
use crate::decode::{Message, decode_message_bytes};
use crate::encode::encode_quote;
use crate::model::Quote;
use crate::subreg::SubRegistry;
use crate::uds::frame::{read_frame, write_frame};

const SNAPSHOT_CHANNEL: usize = 256;

pub async fn run_server(
    socket_path: &str,
    book: Arc<RwLock<Book>>,
    quotes: broadcast::Sender<Quote>,
    registry: Arc<Mutex<SubRegistry>>,
    change_tx: std::sync::mpsc::Sender<()>,
) -> std::io::Result<()> {
    let _ = std::fs::remove_file(socket_path);
    let listener = UnixListener::bind(socket_path)?;
    loop {
        let (stream, _) = listener.accept().await?;
        tokio::spawn(handle_client(
            stream,
            book.clone(),
            quotes.clone(),
            registry.clone(),
            change_tx.clone(),
        ));
    }
}

async fn handle_client(
    stream: UnixStream,
    book: Arc<RwLock<Book>>,
    quotes: broadcast::Sender<Quote>,
    registry: Arc<Mutex<SubRegistry>>,
    change_tx: std::sync::mpsc::Sender<()>,
) {
    let (read_half, write_half) = stream.into_split();
    let subs = Arc::new(Mutex::new(HashSet::<String>::new()));
    let (snap_tx, snap_rx) = mpsc::channel::<Quote>(SNAPSHOT_CHANNEL);
    let writer = tokio::spawn(writer_loop(
        write_half,
        quotes.subscribe(),
        subs.clone(),
        snap_rx,
    ));
    reader_loop(read_half, book, subs, snap_tx, registry, change_tx).await;
    writer.abort();
}

async fn reader_loop(
    mut read_half: OwnedReadHalf,
    book: Arc<RwLock<Book>>,
    subs: Arc<Mutex<HashSet<String>>>,
    snap_tx: mpsc::Sender<Quote>,
    registry: Arc<Mutex<SubRegistry>>,
    change_tx: std::sync::mpsc::Sender<()>,
) {
    while let Ok(Some(frame)) = read_frame(&mut read_half).await {
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
                let snapshots: Vec<Quote> = {
                    let b = book.read().unwrap();
                    syms.iter().filter_map(|sym| b.get(sym).cloned()).collect()
                };
                // try_send (not await): drop the snapshot if the channel is full
                // rather than stalling the reader on a slow consumer — the live
                // broadcast refreshes it on the symbol's next tick.
                for q in snapshots {
                    let _ = snap_tx.try_send(q);
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
    mut quotes: broadcast::Receiver<Quote>,
    subs: Arc<Mutex<HashSet<String>>>,
    mut snap_rx: mpsc::Receiver<Quote>,
) {
    loop {
        tokio::select! {
            live = quotes.recv() => match live {
                Ok(q) => {
                    let want = subs.lock().unwrap().contains(&q.symbol);
                    if want && write_frame(&mut write_half, &encode_quote(&q)).await.is_err() {
                        break;
                    }
                }
                Err(broadcast::error::RecvError::Lagged(_)) => {}
                Err(broadcast::error::RecvError::Closed) => break,
            },
            snap = snap_rx.recv() => match snap {
                Some(q) => {
                    if write_frame(&mut write_half, &encode_quote(&q)).await.is_err() {
                        break;
                    }
                }
                None => break,
            },
        }
    }
}
