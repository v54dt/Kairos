//! Failover proven at the serve layer: two sources for one symbol are injected as
//! decoded events into the shared Book / broadcast + a real `run_server`, and a
//! UDS client is shown to receive ONE coherent source across a failover switch —
//! both on the snapshot path and on the gated live path.
//!
//! A real two-source Aeron E2E is not reachable with the current tools (replayd
//! only remaps the stream id; tapegen hardcodes source 0), so the second source is
//! injected directly, exactly as the poll loop would after decoding it.

use std::sync::{Arc, Mutex, RwLock};
use std::time::Duration;

use kairos_core::book::Book;
use kairos_core::decode::{FeedEvent, decode_quote_bytes};
use kairos_core::encode::{encode_quote, encode_subscribe};
use kairos_core::failover::Selector;
use kairos_core::metrics::Metrics;
use kairos_core::model::{Exchange, PriceLevel, Quote, QuoteBoard, Session};
use kairos_core::poll::{PollDeps, handle_event};
use kairos_core::shutdown::Shutdown;
use kairos_core::subreg::SubRegistry;
use kairos_core::uds::frame::{read_frame, write_frame};
use kairos_core::uds::server::{ServerHandles, run_server};
use tokio::net::UnixStream;
use tokio::sync::broadcast;

fn quote(symbol: &str, source: u16, seq: u64, last: i64) -> Quote {
    Quote {
        symbol: symbol.to_owned(),
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

fn handles(
    book: Arc<RwLock<Book>>,
    tx: broadcast::Sender<FeedEvent>,
    selector: Arc<Selector>,
) -> ServerHandles {
    ServerHandles {
        book,
        quotes: tx,
        registry: Arc::new(Mutex::new(SubRegistry::new())),
        change_tx: std::sync::mpsc::channel::<()>().0,
        metrics: Arc::new(Metrics::default()),
        selector,
    }
}

async fn wait_for_socket(socket: &str) {
    for _ in 0..200 {
        if std::path::Path::new(socket).exists() {
            return;
        }
        tokio::time::sleep(Duration::from_millis(5)).await;
    }
}

async fn next_frame(client: &mut UnixStream) -> Vec<u8> {
    tokio::time::timeout(Duration::from_secs(2), read_frame(client))
        .await
        .expect("timed out")
        .unwrap()
        .unwrap()
}

// Drive the quote through the REAL poll gate exactly as the poll loop would after
// decoding it, so the client only ever sees the active source live.
fn forward(deps: &PollDeps, q: Quote) {
    handle_event(deps, &encode_quote(&q), 0);
}

#[tokio::test]
async fn snapshot_follows_the_active_source_across_a_failover() {
    let socket = format!("/tmp/kairos-failover-snap-{}.sock", std::process::id());
    let book = Arc::new(RwLock::new(Book::new()));
    let (tx, _rx) = broadcast::channel::<FeedEvent>(64);
    let selector = Arc::new(Selector::new(vec![0, 1], 50_000, 200_000));

    // Both feeds hold 2330 at different prices; primary (0) is the active source.
    book.write().unwrap().update(quote("2330", 0, 1, 58_000));
    book.write().unwrap().update(quote("2330", 1, 1, 59_000));
    selector.note(0, 0);
    selector.note(1, 0);
    assert_eq!(selector.eval(0), None);

    let srv_socket = socket.clone();
    let shutdown = Shutdown::new();
    let srv = handles(book.clone(), tx.clone(), selector.clone());
    tokio::spawn(async move {
        let _ = run_server(&srv_socket, srv, shutdown).await;
    });
    wait_for_socket(&socket).await;

    // Client A sees the primary's book.
    let mut a = UnixStream::connect(&socket).await.unwrap();
    write_frame(&mut a, &encode_subscribe(&["2330"]))
        .await
        .unwrap();
    let _ack = next_frame(&mut a).await;
    let snap = decode_quote_bytes(&next_frame(&mut a).await).unwrap();
    assert_eq!(snap.source, 0);
    assert_eq!(snap.last_price, 58_000);

    // Primary goes silent while the secondary keeps receiving -> failover to 1.
    selector.note(1, 100_000);
    assert!(selector.eval(100_000).is_some());
    assert_eq!(selector.active_source(), 1);

    // Client B (new subscribe) now snapshots the secondary's book.
    let mut b = UnixStream::connect(&socket).await.unwrap();
    write_frame(&mut b, &encode_subscribe(&["2330"]))
        .await
        .unwrap();
    let _ack = next_frame(&mut b).await;
    let snap = decode_quote_bytes(&next_frame(&mut b).await).unwrap();
    assert_eq!(snap.source, 1);
    assert_eq!(snap.last_price, 59_000);

    let _ = std::fs::remove_file(&socket);
}

#[tokio::test]
async fn live_stream_serves_only_the_active_source_across_a_failover() {
    let socket = format!("/tmp/kairos-failover-live-{}.sock", std::process::id());
    let book = Arc::new(RwLock::new(Book::new()));
    let (tx, _rx) = broadcast::channel::<FeedEvent>(64);
    let selector = Arc::new(Selector::new(vec![0, 1], 50_000, 200_000));
    selector.note(0, 0);
    selector.note(1, 0);
    assert_eq!(selector.eval(0), None);
    let deps = PollDeps {
        book: book.clone(),
        tx: tx.clone(),
        metrics: Arc::new(Metrics::default()),
        selector: selector.clone(),
    };

    let srv_socket = socket.clone();
    let shutdown = Shutdown::new();
    let srv = handles(book.clone(), tx.clone(), selector.clone());
    tokio::spawn(async move {
        let _ = run_server(&srv_socket, srv, shutdown).await;
    });
    wait_for_socket(&socket).await;

    let mut c = UnixStream::connect(&socket).await.unwrap();
    write_frame(&mut c, &encode_subscribe(&["2330"]))
        .await
        .unwrap();
    let _ack = next_frame(&mut c).await; // book empty -> subAck only, no snapshot

    // Active source 0: the primary's tick is delivered, the secondary's is gated out.
    forward(&deps, quote("2330", 1, 2, 59_100));
    forward(&deps, quote("2330", 0, 2, 58_100));
    let f = decode_quote_bytes(&next_frame(&mut c).await).unwrap();
    assert_eq!(f.source, 0);
    assert_eq!(f.last_price, 58_100);

    // Fail over to the secondary.
    selector.note(1, 100_000);
    assert!(selector.eval(100_000).is_some());

    // Now the secondary is delivered and the primary is gated out.
    forward(&deps, quote("2330", 0, 3, 58_200));
    forward(&deps, quote("2330", 1, 3, 59_200));
    let f = decode_quote_bytes(&next_frame(&mut c).await).unwrap();
    assert_eq!(f.source, 1);
    assert_eq!(f.last_price, 59_200);

    let _ = std::fs::remove_file(&socket);
}
