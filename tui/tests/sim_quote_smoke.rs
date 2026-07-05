//! TUI-reads-sim-quotes smoke: bring up the ISOLATED sim replaying the committed
//! trend_day_2330 tape, drive the REAL `kairos_tui::sources::feed::run` against the
//! sim quote socket, and assert the feed source connects and the 2330 book fills
//! with real quotes — the real-quote integration coverage the tui unit tests lack
//! (no mocked executor). Then tear the sim down and assert no orphan process groups.
//!
//! The sim hub writes no hub-status.json, so the hub_status source has no sim data
//! source and is out of scope here (documented in core/tests/SIM_E2E.md).
//!
//! Ignored by default (spawns the sim daemons + the C++ sim hub). Run:
//!   KAIROS_SIM_BIN=/path/to/kairos-sim KAIROS_SIM_HUBD=/path/to/kairos_sim_hubd \
//!     cargo test -p kairos-tui --test sim_quote_smoke -- --ignored --nocapture

use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use kairos_tui::sources::feed::{self, FeedState};

const SYMBOL: &str = "2330";

fn manifest() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

fn locate(env_var: &str, rel: &str) -> PathBuf {
    if let Ok(p) = std::env::var(env_var)
        && !p.is_empty()
    {
        return PathBuf::from(p);
    }
    for base in [
        manifest().parent().unwrap().to_path_buf(),
        PathBuf::from("/home/coder/Kairos"),
    ] {
        let cand = base.join(rel);
        if cand.is_file() {
            return cand;
        }
    }
    panic!("{rel} not found; set {env_var} to its path");
}

fn wait_for(path: &Path, timeout: Duration) -> bool {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if path.exists() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    false
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

#[test]
#[ignore = "spawns the sim daemons + C++ sim hub; run with --ignored"]
fn tui_feed_source_receives_sim_quotes() {
    let tag = std::process::id();
    let base = std::env::temp_dir().join(format!("kairos-tui-e2e-{tag}"));
    let _ = std::fs::remove_dir_all(&base);
    std::fs::create_dir_all(&base).unwrap();

    let aeron_dir = format!("/dev/shm/aeron-tui-e2e-{tag}");
    let quote_sock = base.join("q.sock");
    let order_sock = base.join("o.sock");
    let pidfile = PathBuf::from(format!("{aeron_dir}.kairos-sim.pids"));

    let sim_bin = locate("KAIROS_SIM_BIN", "core/target/release/kairos-sim");
    let hubd = locate("KAIROS_SIM_HUBD", "exec/scenario/build/kairos_sim_hubd");
    let tape = manifest()
        .parent()
        .unwrap()
        .join("core/tests/fixtures/tapes/trend_day_2330.kqr");
    assert!(
        tape.is_file(),
        "trend_day tape fixture missing: {}",
        tape.display()
    );

    let mut sim = Command::new(&sim_bin)
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

    let state = Arc::new(Mutex::new(FeedState::default()));
    let rt = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap();
    let sock = quote_sock.to_string_lossy().to_string();
    let feed_state = state.clone();
    let handle = rt.spawn(feed::run(sock, vec![SYMBOL.to_string()], feed_state));

    // Poll the shared FeedState until the real source has connected and applied at
    // least one 2330 quote from the sim quote socket.
    let ok = rt.block_on(async {
        let deadline = Instant::now() + Duration::from_secs(15);
        while Instant::now() < deadline {
            {
                let s = state.lock().unwrap();
                let source_count: u64 = s.per_source.values().map(|st| st.count).sum();
                if s.connected && s.per_symbol.contains_key(SYMBOL) && source_count > 0 {
                    return true;
                }
            }
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
        false
    });
    handle.abort();

    {
        let s = state.lock().unwrap();
        let source_count: u64 = s.per_source.values().map(|st| st.count).sum();
        assert!(
            ok,
            "tui feed source never received a 2330 quote from the sim"
        );
        assert!(s.per_symbol.contains_key(SYMBOL), "2330 book not populated");
        println!(
            "sim_quote_smoke: tui feed connected={}, {} quotes, 2330 last={}",
            s.connected, source_count, s.per_symbol[SYMBOL].last_price
        );
    }

    // Tear the sim down and prove every recorded child process group is gone.
    assert!(
        wait_for(&pidfile, Duration::from_secs(5)),
        "sim pidfile never written"
    );
    let pgids: Vec<i32> = std::fs::read_to_string(&pidfile)
        .unwrap()
        .lines()
        .filter_map(|l| l.split_once(' ').and_then(|(p, _)| p.trim().parse().ok()))
        .collect();
    assert!(!pgids.is_empty(), "no sim child pgids recorded");

    // SAFETY: signalling the sim pid we own; its guard tears down every child.
    unsafe { libc::kill(sim.id() as i32, libc::SIGTERM) };
    assert!(
        wait_child(&mut sim, Duration::from_secs(10)),
        "kairos-sim did not exit after SIGTERM"
    );

    std::thread::sleep(Duration::from_millis(300));
    for pgid in &pgids {
        // SAFETY: killpg(pgid, 0) probes liveness only; ESRCH => the group is gone.
        let rc = unsafe { libc::killpg(*pgid, 0) };
        assert_eq!(
            rc, -1,
            "orphan process group {pgid} still alive after teardown"
        );
    }
    assert!(!pidfile.exists(), "sim pidfile not cleaned up on teardown");

    println!("sim_quote_smoke: teardown clean, no orphan process groups");
    let _ = std::fs::remove_dir_all(&base);
    let _ = std::fs::remove_dir_all(&aeron_dir);
}
