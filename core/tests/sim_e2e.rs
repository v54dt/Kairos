//! End-to-end trader drill: replay the committed trend_day_2330 tape through an
//! ISOLATED sim pipeline (own Aeron dir + sim sockets), run the REAL C++
//! kairos_scenario_trader --live against the sim order/quote sockets, and assert
//! the full flow:
//!   (a) the trader FILLS (acks + fills, not acked-but-0-fill),
//!   (b) the B2 order journal records the fsynced fills,
//!   (c) the trader ends cleanly on SIGTERM, and
//!   (d) teardown leaves NO orphan process groups (every sim child pgid is gone).
//! Touches only the sim namespace, never the live pipeline.
//!
//! Ignored by default (spawns the sim daemons + the C++ sim hub + the trader). Run:
//!   KAIROS_SIM_HUBD=/path/to/kairos_sim_hubd \
//!     KAIROS_SCENARIO_TRADER=/path/to/kairos_scenario_trader \
//!     cargo test --test sim_e2e -- --ignored --nocapture

use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, Instant};

const SYMBOL: &str = "2330";

fn manifest() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

fn locate_bin(env_var: &str, rel: &str) -> PathBuf {
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

/// Return the first fill line's `shares` value found in any `.jsonl` under `dir`,
/// or None if no fill has been journaled yet.
fn journal_fill_shares(dir: &Path) -> Option<i64> {
    let entries = std::fs::read_dir(dir).ok()?;
    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().and_then(|e| e.to_str()) != Some("jsonl") {
            continue;
        }
        let text = std::fs::read_to_string(&path).unwrap_or_default();
        for line in text.lines() {
            if !line.contains("\"type\":\"fill\"") {
                continue;
            }
            if let Some(shares) = parse_json_int(line, "shares")
                && shares > 0
            {
                return Some(shares);
            }
        }
    }
    None
}

fn parse_json_int(line: &str, key: &str) -> Option<i64> {
    let needle = format!("\"{key}\":");
    let start = line.find(&needle)? + needle.len();
    let rest = &line[start..];
    let end = rest
        .find(|c: char| !c.is_ascii_digit() && c != '-')
        .unwrap_or(rest.len());
    rest[..end].parse().ok()
}

#[test]
#[ignore = "spawns the sim daemons + C++ sim hub + trader; run with --ignored"]
fn trader_fills_journal_and_leaves_no_orphans() {
    let tag = std::process::id();
    let base = std::env::temp_dir().join(format!("kairos-e2e-{tag}"));
    let _ = std::fs::remove_dir_all(&base);
    std::fs::create_dir_all(&base).unwrap();
    let journal_dir = base.join("journal");
    std::fs::create_dir_all(&journal_dir).unwrap();

    let aeron_dir = format!("/dev/shm/aeron-e2e-{tag}");
    let quote_sock = base.join("q.sock");
    let order_sock = base.join("o.sock");
    let pidfile = PathBuf::from(format!("{aeron_dir}.kairos-sim.pids"));
    let blacklist = base.join("no-blacklist.csv"); // deliberately absent -> gate refuses

    let tape = manifest().join("tests/fixtures/tapes/trend_day_2330.kqr");
    assert!(
        tape.is_file(),
        "trend_day tape fixture missing: {}",
        tape.display()
    );

    // Per-run scenario copy with the journal dir injected (the fixture omits it to
    // avoid a machine-specific absolute path).
    let fixture = manifest().join("tests/fixtures/scenarios/trend_day_2330_drill.toml");
    let mut toml = std::fs::read_to_string(&fixture).unwrap();
    toml.push_str(&format!(
        "\n[journal]\ndir = \"{}\"\n",
        journal_dir.display()
    ));
    let scenario = base.join("scenario.toml");
    std::fs::write(&scenario, toml).unwrap();

    let hubd = locate_bin("KAIROS_SIM_HUBD", "exec/scenario/build/kairos_sim_hubd");
    let trader = locate_bin(
        "KAIROS_SCENARIO_TRADER",
        "exec/scenario/build/kairos_scenario_trader",
    );
    let sim_bin = env!("CARGO_BIN_EXE_kairos-sim");

    let mut sim = Command::new(sim_bin)
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
    assert!(
        wait_for(&order_sock, Duration::from_secs(20)),
        "sim order socket never appeared"
    );

    let mut trader_proc = Command::new(&trader)
        .arg(&scenario)
        .args(["--live", "--ignore-window", "--ignore-blacklist", "--yes"])
        .env("KAIROS_QUOTE_SOCK", &quote_sock)
        .env("KAIROS_ORDER_SOCK", &order_sock)
        .env("KAIROS_BLACKLIST_CSV", &blacklist)
        .stdin(std::process::Stdio::null())
        .spawn()
        .expect("spawn kairos_scenario_trader");

    // (a)+(b): the trader must ACK and FILL, and the fill must land fsynced in the
    // B2 journal. Poll the journal dir until a fill line with shares > 0 appears.
    let deadline = Instant::now() + Duration::from_secs(60);
    let mut filled = None;
    while Instant::now() < deadline {
        if let Some(shares) = journal_fill_shares(&journal_dir) {
            filled = Some(shares);
            break;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    let shares = filled.expect("trader never journaled a fill (acked-but-0-fill or no journal)");
    assert!(
        shares > 0,
        "journaled fill has non-positive shares: {shares}"
    );
    println!("sim_e2e: trader filled {shares} sh (first journaled fill)");

    // (c): the trader ends cleanly on SIGTERM.
    // SAFETY: signalling a child pid we own.
    unsafe { libc::kill(trader_proc.id() as i32, libc::SIGTERM) };
    assert!(
        wait_child(&mut trader_proc, Duration::from_secs(15)),
        "trader did not exit cleanly after SIGTERM"
    );

    // The journal must still hold the fsynced fill after the trader is gone.
    assert!(
        journal_fill_shares(&journal_dir).is_some(),
        "journal fill lost after trader exit"
    );

    // (d): tear the sim down and prove every recorded child process group is gone.
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

    // Final belt: no trader/hub process still references this drill's base dir.
    let leaks = Command::new("pgrep")
        .arg("-f")
        .arg(base.to_string_lossy().to_string())
        .output()
        .expect("run pgrep");
    assert!(
        leaks.stdout.is_empty(),
        "orphan process referencing the drill base survived: {}",
        String::from_utf8_lossy(&leaks.stdout)
    );

    println!("sim_e2e: teardown clean, no orphan process groups");
    let _ = std::fs::remove_dir_all(&base);
    let _ = std::fs::remove_dir_all(&aeron_dir);
}
