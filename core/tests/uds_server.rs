use std::sync::{Arc, Mutex, RwLock};
use std::time::Duration;

use kairos_core::book::Book;
use kairos_core::decode::{FeedEvent, decode_quote_bytes};
use kairos_core::encode::encode_subscribe;
use kairos_core::failover::Selector;
use kairos_core::metrics::Metrics;
use kairos_core::model::{Exchange, PriceLevel, Quote, QuoteBoard, Session, Trade};
use kairos_core::subreg::SubRegistry;
use kairos_core::uds::frame::{read_frame, write_frame};
use kairos_core::uds::server::{ServerHandles, run_server};
use tokio::net::UnixStream;
use tokio::sync::{broadcast, watch};

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
        source: 0,
        seq: 0,
        epoch: 0,
        recv_ts_us: 0,
        board: QuoteBoard::RoundLot,
        session: Session::Unknown,
        trading_date: 0,
        simtrade: false,
        underlying_price: 0,
    }
}

fn trade(symbol: &str, price: i64) -> Trade {
    Trade {
        symbol: symbol.to_owned(),
        exchange: Exchange::Twse,
        source: 0,
        seq: 0,
        epoch: 0,
        trade_ts_us: 0,
        recv_ts_us: 0,
        price_mantissa: price,
        price_scale: 2,
        volume: 5,
        is_trial: false,
        session: Session::Unknown,
        trading_date: 0,
        simtrade: false,
        underlying_price: 0,
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

fn decode_trade_frame(frame: &[u8]) -> Trade {
    match kairos_core::decode::decode_feed_event(frame).unwrap() {
        FeedEvent::Trade(t) => t,
        other => panic!("expected a trade frame, got {other:?}"),
    }
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
    let (tx, _rx) = broadcast::channel::<FeedEvent>(16);
    let registry = Arc::new(Mutex::new(SubRegistry::new()));
    let (change_tx, _change_rx) = std::sync::mpsc::channel::<()>();

    book.write().unwrap().update(quote("2330", 58000));

    let srv_book = book.clone();
    let srv_tx = tx.clone();
    let srv_socket = socket.clone();
    let (_shutdown_tx, shutdown_rx) = tokio::sync::watch::channel(false);
    tokio::spawn(async move {
        let handles = ServerHandles {
            book: srv_book,
            quotes: srv_tx,
            registry,
            change_tx,
            metrics: std::sync::Arc::new(Metrics::default()),
            selector: std::sync::Arc::new(Selector::new(vec![0], 2_000_000, 5_000_000)),
        };
        let _ = run_server(&srv_socket, handles, shutdown_rx).await;
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
    tx.send(FeedEvent::Quote(quote("2317", 11000))).unwrap();
    tx.send(FeedEvent::Quote(quote("2330", 58100))).unwrap();

    let live = next_quote(&mut client).await;
    assert_eq!(live.symbol, "2330");
    assert_eq!(live.last_price, 58100);

    // a Trade for the subscribed symbol reaches the client on the same stream;
    // an unsubscribed symbol's Trade is filtered out.
    tx.send(FeedEvent::Trade(trade("2317", 11050))).unwrap();
    tx.send(FeedEvent::Trade(trade("2330", 58150))).unwrap();
    let tr = decode_trade_frame(&next_frame(&mut client).await);
    assert_eq!(tr.symbol, "2330");
    assert_eq!(tr.price_mantissa, 58150);
    assert_eq!(tr.volume, 5);

    let _ = std::fs::remove_file(&socket);
}

#[tokio::test]
async fn subscribe_refcounts_and_disconnect_releases() {
    let socket = format!("/tmp/kairos-uds-reg-test-{}.sock", std::process::id());
    let book = Arc::new(RwLock::new(Book::new()));
    let (tx, _rx) = broadcast::channel::<FeedEvent>(16);
    let registry = Arc::new(Mutex::new(SubRegistry::new()));
    let (change_tx, change_rx) = std::sync::mpsc::channel::<()>();

    let srv_book = book.clone();
    let srv_tx = tx.clone();
    let srv_socket = socket.clone();
    let srv_reg = registry.clone();
    let (_shutdown_tx, shutdown_rx) = tokio::sync::watch::channel(false);
    tokio::spawn(async move {
        let handles = ServerHandles {
            book: srv_book,
            quotes: srv_tx,
            registry: srv_reg,
            change_tx,
            metrics: std::sync::Arc::new(Metrics::default()),
            selector: std::sync::Arc::new(Selector::new(vec![0], 2_000_000, 5_000_000)),
        };
        let _ = run_server(&srv_socket, handles, shutdown_rx).await;
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

// A shutdown that flips WHILE frames are being written to a slow reader must never cut
// a length-prefixed frame: the client drains only whole frames and then a clean EOF.
// read_frame returns Err on a mid-payload EOF, so "every read Ok, terminating in
// Ok(None)" is exactly the proof that no frame was truncated at the shutdown cut.
#[tokio::test]
async fn shutdown_delivers_whole_frames_then_eof_to_a_slow_reader() {
    let socket = format!("/tmp/kairos-uds-shutdown-test-{}.sock", std::process::id());
    let book = Arc::new(RwLock::new(Book::new()));
    let (tx, _rx) = broadcast::channel::<FeedEvent>(4096);
    let registry = Arc::new(Mutex::new(SubRegistry::new()));
    let (change_tx, _change_rx) = std::sync::mpsc::channel::<()>();
    let (shutdown_tx, shutdown_rx) = watch::channel(false);

    let srv_book = book.clone();
    let srv_tx = tx.clone();
    let srv_socket = socket.clone();
    let server = tokio::spawn(async move {
        let handles = ServerHandles {
            book: srv_book,
            quotes: srv_tx,
            registry,
            change_tx,
            metrics: std::sync::Arc::new(Metrics::default()),
            selector: std::sync::Arc::new(Selector::new(vec![0], 2_000_000, 5_000_000)),
        };
        let _ = run_server(&srv_socket, handles, shutdown_rx).await;
    });

    wait_for_socket(&socket).await;

    let mut client = UnixStream::connect(&socket).await.unwrap();
    write_frame(&mut client, &encode_subscribe(&["2330"]))
        .await
        .unwrap();

    // Push a long burst of subscribed-symbol frames so the writer is mid-stream (and,
    // against a slow reader, blocked inside a socket write) when shutdown flips.
    let producer = tokio::spawn(async move {
        for i in 0..5000 {
            if tx.send(FeedEvent::Quote(quote("2330", 58000 + i))).is_err() {
                break;
            }
            if i % 500 == 0 {
                tokio::task::yield_now().await;
            }
        }
    });

    // Read a few frames slowly, then trigger shutdown mid-stream and keep draining.
    let mut frames = 0usize;
    let mut hit_eof = false;
    for read_n in 0..100_000 {
        if read_n == 20 {
            shutdown_tx.send(true).unwrap();
        }
        if read_n % 5 == 0 {
            tokio::time::sleep(Duration::from_millis(1)).await;
        }
        match tokio::time::timeout(Duration::from_secs(5), read_frame(&mut client)).await {
            Ok(Ok(Some(frame))) => {
                assert!(!frame.is_empty(), "a whole frame is never zero-length");
                frames += 1;
            }
            Ok(Ok(None)) => {
                hit_eof = true;
                break;
            }
            Ok(Err(e)) => panic!("frame was cut at shutdown (partial read): {e:?}"),
            Err(_) => panic!("timed out draining frames after shutdown"),
        }
    }

    assert!(
        hit_eof,
        "server must close the connection (EOF) after draining"
    );
    assert!(frames > 0, "client should have received whole frames");

    let _ = producer.await;
    // The server drains all clients and returns cleanly (the exit-0 path).
    tokio::time::timeout(Duration::from_secs(5), server)
        .await
        .expect("run_server did not return after shutdown")
        .unwrap();

    let _ = std::fs::remove_file(&socket);
}

// A client that subscribes and then never reads wedges its writer inside write_frame
// once the kernel send buffer fills — past the shutdown-watch select point. Shutdown
// must still return in bounded time by aborting that straggler, or the whole process
// hangs until systemd SIGKILLs it. This proves run_server returns without external help.
#[tokio::test]
async fn shutdown_is_bounded_even_with_a_wedged_non_reading_client() {
    let socket = format!("/tmp/kairos-uds-wedged-test-{}.sock", std::process::id());
    let book = Arc::new(RwLock::new(Book::new()));
    let (tx, _rx) = broadcast::channel::<FeedEvent>(8192);
    let registry = Arc::new(Mutex::new(SubRegistry::new()));
    let (change_tx, _change_rx) = std::sync::mpsc::channel::<()>();
    let (shutdown_tx, shutdown_rx) = watch::channel(false);

    let srv_book = book.clone();
    let srv_tx = tx.clone();
    let srv_socket = socket.clone();
    let server = tokio::spawn(async move {
        let handles = ServerHandles {
            book: srv_book,
            quotes: srv_tx,
            registry,
            change_tx,
            metrics: std::sync::Arc::new(Metrics::default()),
            selector: std::sync::Arc::new(Selector::new(vec![0], 2_000_000, 5_000_000)),
        };
        let _ = run_server(&srv_socket, handles, shutdown_rx).await;
    });

    wait_for_socket(&socket).await;

    // Subscribe, then deliberately never read: hold the client so the socket stays open.
    let mut client = UnixStream::connect(&socket).await.unwrap();
    write_frame(&mut client, &encode_subscribe(&["2330"]))
        .await
        .unwrap();

    // Flood subscribed frames until the kernel send buffer fills and the server's writer
    // parks inside write_frame (past its shutdown select).
    for i in 0..20_000 {
        if tx.send(FeedEvent::Quote(quote("2330", 58000 + i))).is_err() {
            break;
        }
    }
    tokio::time::sleep(Duration::from_millis(200)).await;

    shutdown_tx.send(true).unwrap();

    // Bounded exit: the drain grace is 3s, so allow a margin. Before the fix this hung
    // until the non-reading client drained (i.e. indefinitely).
    tokio::time::timeout(Duration::from_secs(6), server)
        .await
        .expect("run_server did not return within the bounded shutdown grace")
        .unwrap();

    drop(client);
    let _ = std::fs::remove_file(&socket);
}
