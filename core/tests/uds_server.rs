use std::sync::{Arc, Mutex, RwLock};
use std::time::Duration;

use kairos_core::book::Book;
use kairos_core::decode::decode_quote_bytes;
use kairos_core::encode::encode_subscribe;
use kairos_core::metrics::Metrics;
use kairos_core::model::{Exchange, PriceLevel, Quote};
use kairos_core::subreg::SubRegistry;
use kairos_core::uds::frame::{read_frame, write_frame};
use kairos_core::uds::server::run_server;
use tokio::net::UnixStream;
use tokio::sync::broadcast;

fn quote(symbol: &str, last: i64) -> Quote {
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
    }
}

async fn next_frame(client: &mut UnixStream) -> Vec<u8> {
    tokio::time::timeout(Duration::from_secs(2), read_frame(client))
        .await
        .expect("timed out waiting for frame")
        .unwrap()
        .unwrap()
}

async fn next_quote(client: &mut UnixStream) -> Quote {
    decode_quote_bytes(&next_frame(client).await).unwrap()
}

async fn wait_for_socket(socket: &str) {
    for _ in 0..100 {
        if std::path::Path::new(socket).exists() {
            return;
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}

#[tokio::test]
async fn uds_snapshot_then_live_push_with_filtering() {
    let socket = format!("/tmp/kairos-uds-test-{}.sock", std::process::id());
    let book = Arc::new(RwLock::new(Book::new()));
    let (tx, _rx) = broadcast::channel::<Quote>(16);
    let registry = Arc::new(Mutex::new(SubRegistry::new()));
    let (change_tx, _change_rx) = std::sync::mpsc::channel::<()>();

    book.write().unwrap().update(quote("2330", 58000));

    let srv_book = book.clone();
    let srv_tx = tx.clone();
    let srv_socket = socket.clone();
    tokio::spawn(async move {
        let _ = run_server(
            &srv_socket,
            srv_book,
            srv_tx,
            registry,
            change_tx,
            std::sync::Arc::new(Metrics::default()),
        )
        .await;
    });

    wait_for_socket(&socket).await;

    let mut client = UnixStream::connect(&socket).await.unwrap();
    write_frame(&mut client, &encode_subscribe(&["2330"]))
        .await
        .unwrap();

    // subscribe is acked first (a control frame, not a quote), then the snapshot
    let ack = next_frame(&mut client).await;
    assert!(
        decode_quote_bytes(&ack).is_err(),
        "expected a subAck before the snapshot"
    );

    let snap = next_quote(&mut client).await;
    assert_eq!(snap.symbol, "2330");
    assert_eq!(snap.last_price, 58000);

    // live: 2317 is not subscribed (filtered), 2330 is delivered
    tx.send(quote("2317", 11000)).unwrap();
    tx.send(quote("2330", 58100)).unwrap();

    let live = next_quote(&mut client).await;
    assert_eq!(live.symbol, "2330");
    assert_eq!(live.last_price, 58100);

    let _ = std::fs::remove_file(&socket);
}

#[tokio::test]
async fn subscribe_refcounts_and_disconnect_releases() {
    let socket = format!("/tmp/kairos-uds-reg-test-{}.sock", std::process::id());
    let book = Arc::new(RwLock::new(Book::new()));
    let (tx, _rx) = broadcast::channel::<Quote>(16);
    let registry = Arc::new(Mutex::new(SubRegistry::new()));
    let (change_tx, change_rx) = std::sync::mpsc::channel::<()>();

    let srv_book = book.clone();
    let srv_tx = tx.clone();
    let srv_socket = socket.clone();
    let srv_reg = registry.clone();
    tokio::spawn(async move {
        let _ = run_server(
            &srv_socket,
            srv_book,
            srv_tx,
            srv_reg,
            change_tx,
            std::sync::Arc::new(Metrics::default()),
        )
        .await;
    });

    wait_for_socket(&socket).await;

    let mut client = UnixStream::connect(&socket).await.unwrap();
    write_frame(&mut client, &encode_subscribe(&["2330"]))
        .await
        .unwrap();

    // the subscription enters the global desired set
    let mut registered = false;
    for _ in 0..200 {
        if registry.lock().unwrap().desired() == vec!["2330".to_string()] {
            registered = true;
            break;
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    assert!(registered, "registry should contain 2330 after subscribe");
    assert!(
        change_rx.try_recv().is_ok(),
        "subscribe should signal a change"
    );

    // disconnecting the only subscriber releases the refcount
    drop(client);
    let mut released = false;
    for _ in 0..200 {
        if registry.lock().unwrap().desired().is_empty() {
            released = true;
            break;
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
    assert!(
        released,
        "registry should be empty after client disconnects"
    );

    let _ = std::fs::remove_file(&socket);
}
