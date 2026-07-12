// A shutdown that fires BEFORE run_server subscribes must still stop the accept
// loop: run_server subscribes internally (uds/server.rs), and a subscriber created
// after set() must observe the signal rather than block forever.
use std::sync::{Arc, RwLock};
use std::time::Duration;

use kairos_core::book::Book;
use kairos_core::decode::FeedEvent;
use kairos_core::shutdown::Shutdown;
use kairos_core::uds::server::{ServerHandles, run_server};
use tokio::sync::broadcast;

#[tokio::test]
async fn shutdown_set_before_server_start_still_stops_accept_loop() {
    let socket = format!("/tmp/kairos-prestart-{}.sock", std::process::id());
    let book = Arc::new(RwLock::new(Book::new()));
    let (tx, _rx) = broadcast::channel::<FeedEvent>(16);
    let handles = ServerHandles::builder(book, tx).build();

    let shutdown = Shutdown::new();
    shutdown.set(); // SIGTERM arrives during startup, before run_server subscribes

    let res = tokio::time::timeout(
        Duration::from_secs(3),
        run_server(&socket, handles, shutdown),
    )
    .await;
    let _ = std::fs::remove_file(&socket);
    assert!(
        res.is_ok(),
        "run_server hung: a shutdown set before it subscribed was missed by the accept loop"
    );
}
