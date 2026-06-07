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
use crate::uds::frame::{read_frame, write_frame};

pub async fn run_server(
    socket_path: &str,
    book: Arc<RwLock<Book>>,
    quotes: broadcast::Sender<Quote>,
) -> std::io::Result<()> {
    let _ = std::fs::remove_file(socket_path);
    let listener = UnixListener::bind(socket_path)?;
    loop {
        let (stream, _) = listener.accept().await?;
        tokio::spawn(handle_client(stream, book.clone(), quotes.clone()));
    }
}

async fn handle_client(
    stream: UnixStream,
    book: Arc<RwLock<Book>>,
    quotes: broadcast::Sender<Quote>,
) {
    let (read_half, write_half) = stream.into_split();
    let subs = Arc::new(Mutex::new(HashSet::<String>::new()));
    let (snap_tx, snap_rx) = mpsc::unbounded_channel::<Quote>();
    let writer = tokio::spawn(writer_loop(
        write_half,
        quotes.subscribe(),
        subs.clone(),
        snap_rx,
    ));
    reader_loop(read_half, book, subs, snap_tx).await;
    writer.abort();
}

async fn reader_loop(
    mut read_half: OwnedReadHalf,
    book: Arc<RwLock<Book>>,
    subs: Arc<Mutex<HashSet<String>>>,
    snap_tx: mpsc::UnboundedSender<Quote>,
) {
    while let Ok(Some(frame)) = read_frame(&mut read_half).await {
        match decode_message_bytes(&frame) {
            Ok(Message::Subscribe(syms)) => {
                {
                    let mut s = subs.lock().unwrap();
                    for sym in &syms {
                        s.insert(sym.clone());
                    }
                }
                let snapshots: Vec<Quote> = {
                    let b = book.read().unwrap();
                    syms.iter().filter_map(|sym| b.get(sym).cloned()).collect()
                };
                for q in snapshots {
                    let _ = snap_tx.send(q);
                }
            }
            Ok(Message::Unsubscribe(syms)) => {
                let mut s = subs.lock().unwrap();
                for sym in &syms {
                    s.remove(sym);
                }
            }
            _ => {}
        }
    }
}

async fn writer_loop(
    mut write_half: OwnedWriteHalf,
    mut quotes: broadcast::Receiver<Quote>,
    subs: Arc<Mutex<HashSet<String>>>,
    mut snap_rx: mpsc::UnboundedReceiver<Quote>,
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
