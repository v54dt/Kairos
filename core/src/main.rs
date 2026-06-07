use std::sync::{Arc, RwLock};
use std::thread;

use kairos_core::book::Book;
use kairos_core::decode::decode_quote_bytes;
use kairos_core::ipc::aeron::{AeronSub, DEFAULT_STREAM_ID};
use kairos_core::model::Quote;
use kairos_core::uds::server::run_server;
use tokio::sync::broadcast;

const SOCKET_PATH: &str = "/tmp/kairos-quotes.sock";

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let book = Arc::new(RwLock::new(Book::new()));
    let (tx, _) = broadcast::channel::<Quote>(1024);

    let aeron_book = book.clone();
    let aeron_tx = tx.clone();
    thread::spawn(move || aeron_poll_loop(aeron_book, aeron_tx));

    eprintln!("kairos-core: UDS quote server on {SOCKET_PATH}");
    run_server(SOCKET_PATH, book, tx).await?;
    Ok(())
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
