//! End-to-end kairos-sim test: replay a tiny synthetic KQR tape through an
//! ISOLATED sim pipeline, assert a UDS client on the SIM quote socket receives
//! quotes, then SIGTERM kairos-sim and assert every child process group is gone
//! (no orphans). Touches only the sim namespace, never the live pipeline.
//!
//! Ignored by default (spawns the sim daemons + the C++ sim hub). Run with:
//!   KAIROS_SIM_HUBD=/path/to/kairos_sim_hubd \
//!     cargo test --test sim_roundtrip -- --ignored --nocapture

use std::path::PathBuf;
use std::process::Command;
use std::time::{Duration, Instant};

use kairos_core::encode::{encode_quote, encode_subscribe};
use kairos_core::model::{Exchange, PriceLevel, Quote, QuoteBoard, Session};
use kairos_core::record::{FileHeader, RecordWriter};
use kairos_core::uds::frame::{read_frame, write_frame};
use tokio::net::UnixStream;

const SYMBOL: &str = "2330";
const N: usize = 60;

fn quote(i: usize) -> Quote {
    let bid = 100_000 + i as i64;
    Quote {
        symbol: SYMBOL.to_owned(),
        exchange: Exchange::Twse,
        quote_ts_us: i as i64 * 10_000,
        bids: vec![PriceLevel {
            price_mantissa: bid,
            price_scale: 2,
            volume: 10,
        }],
        asks: vec![PriceLevel {
            price_mantissa: bid + 100,
            price_scale: 2,
            volume: 10,
        }],
        last_price: bid,
        last_scale: 2,
        last_volume: 5,
        is_trial: false,
        source: 1,
        seq: i as u64,
        epoch: 0,
        recv_ts_us: i as i64 * 10_000,
        board: QuoteBoard::RoundLot,
        session: Session::Day,
        trading_date: 20_260_101,
        simtrade: false,
        underlying_price: 0,
    }
}

fn write_tape(path: &std::path::Path) {
    let f = std::fs::File::create(path).unwrap();
    let mut w =
        RecordWriter::create(std::io::BufWriter::new(f), &FileHeader::new(1001, 0)).unwrap();
    for i in 0..N {
        w.append(i as i64 * 10_000, &encode_quote(&quote(i)))
            .unwrap();
    }
    w.flush().unwrap();
}

fn locate_hubd() -> PathBuf {
    if let Ok(p) = std::env::var("KAIROS_SIM_HUBD") {
        return PathBuf::from(p);
    }
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    for base in [
        manifest.parent().unwrap().to_path_buf(),
        PathBuf::from("/home/coder/Kairos"),
    ] {
        let cand = base.join("exec/scenario/build/kairos_sim_hubd");
        if cand.is_file() {
            return cand;
        }
    }
    panic!("kairos_sim_hubd not found; set KAIROS_SIM_HUBD to its path");
}

fn wait_for(path: &std::path::Path, timeout: Duration) -> bool {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if path.exists() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    false
}

async fn collect_quotes(sock: &std::path::Path, want: usize, timeout: Duration) -> usize {
    let mut stream = UnixStream::connect(sock)
        .await
        .expect("connect sim quote sock");
    write_frame(&mut stream, &encode_subscribe(&[SYMBOL]))
        .await
        .unwrap();
    let mut got = 0usize;
    let deadline = Instant::now() + timeout;
    while got < want && Instant::now() < deadline {
        match tokio::time::timeout(Duration::from_millis(500), read_frame(&mut stream)).await {
            Ok(Ok(Some(_frame))) => got += 1,
            Ok(Ok(None)) => break,
            Ok(Err(_)) => break,
            Err(_) => {}
        }
    }
    got
}

#[test]
#[ignore = "spawns the sim daemons + C++ sim hub; run with --ignored"]
fn replay_into_sim_delivers_quotes_and_leaves_no_orphans() {
    let tag = std::process::id();
    let base = std::env::temp_dir().join(format!("kairos-simtest-{tag}"));
    let _ = std::fs::remove_dir_all(&base);
    std::fs::create_dir_all(&base).unwrap();

    let tape = base.join("tape.kqr");
    write_tape(&tape);

    let aeron_dir = format!("/dev/shm/aeron-simtest-{tag}");
    let quote_sock = base.join("q.sock");
    let order_sock = base.join("o.sock");
    let pidfile = PathBuf::from(format!("{aeron_dir}.kairos-sim.pids"));
    let hubd = locate_hubd();
    let sim_bin = env!("CARGO_BIN_EXE_kairos-sim");

    let mut child = Command::new(sim_bin)
        .arg("replay")
        .arg(&tape)
        .args(["--symbols", SYMBOL, "--speed", "8"])
        .env("KAIROS_SIM_AERON_DIR", &aeron_dir)
        .env("KAIROS_SIM_QUOTE_SOCK", &quote_sock)
        .env("KAIROS_SIM_ORDER_SOCK", &order_sock)
        .env("KAIROS_SIM_HUBD", &hubd)
        .spawn()
        .expect("spawn kairos-sim");

    assert!(
        wait_for(&quote_sock, Duration::from_secs(20)),
        "sim quote socket never appeared"
    );

    let rt = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap();
    let got = rt.block_on(collect_quotes(&quote_sock, N, Duration::from_secs(15)));
    println!("sim_roundtrip: received {got} quotes on the sim quote socket");
    assert!(got >= 3, "expected quotes from the replay, got {got}");

    // Capture the child process groups before teardown so we can prove they die.
    assert!(
        wait_for(&pidfile, Duration::from_secs(5)),
        "pidfile never written"
    );
    let pgids: Vec<i32> = std::fs::read_to_string(&pidfile)
        .unwrap()
        .lines()
        .filter_map(|l| l.split_once(' ').and_then(|(p, _)| p.trim().parse().ok()))
        .collect();
    assert!(!pgids.is_empty(), "no child pgids recorded");

    // SIGTERM kairos-sim; its guard must tear down every child.
    unsafe { libc::kill(child.id() as i32, libc::SIGTERM) };
    let status = wait_child(&mut child, Duration::from_secs(10));
    assert!(status, "kairos-sim did not exit after SIGTERM");

    std::thread::sleep(Duration::from_millis(300));
    for pgid in &pgids {
        // killpg(pgid, 0) must fail with ESRCH: the whole group is gone.
        let rc = unsafe { libc::killpg(*pgid, 0) };
        assert_eq!(rc, -1, "orphan process group {pgid} still alive");
    }
    assert!(!pidfile.exists(), "pidfile not cleaned up on teardown");

    let _ = std::fs::remove_dir_all(&base);
    let _ = std::fs::remove_dir_all(&aeron_dir);
}

fn wait_child(child: &mut std::process::Child, timeout: Duration) -> bool {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if matches!(child.try_wait(), Ok(Some(_))) {
            return true;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    let _ = child.kill();
    false
}
