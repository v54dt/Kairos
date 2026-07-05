//! Regression: a SIGTERM/SIGINT during the STARTUP window — after driver/core/hubd
//! are spawned but before `wait_ready` completes — must still tear every child down.
//! The signal handler is installed before the first spawn, so the signal unwinds
//! through ChildGuard::drop instead of killing kairos-sim by default disposition and
//! orphaning the daemons ("no orphaned sim daemons ever").
//!
//! #[ignore]: spawns processes and sends signals. Run with:
//!   cargo test --test sim_orphan_startup -- --ignored --nocapture

use std::process::Command;
use std::time::{Duration, Instant};

fn count_sentinel(sentinel: &str) -> usize {
    let out = Command::new("pgrep")
        .arg("-fc")
        .arg(format!("sleep {sentinel}"))
        .output()
        .unwrap();
    String::from_utf8_lossy(&out.stdout).trim().parse().unwrap_or(0)
}

fn write_fake(path: &std::path::Path, sentinel: &str) {
    use std::os::unix::fs::PermissionsExt;
    std::fs::write(path, format!("#!/bin/sh\nexec sleep {sentinel}\n")).unwrap();
    std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o755)).unwrap();
}

#[test]
#[ignore = "spawns processes + signals; red-team orphan repro"]
fn sigterm_during_startup_orphans_children() {
    let tag = std::process::id();
    let sentinel = format!("{}", 90_000_000 + tag % 1_000_000);
    let base = std::env::temp_dir().join(format!("kairos-sim-orphan-{tag}"));
    let _ = std::fs::remove_dir_all(&base);
    let bins = base.join("bins");
    std::fs::create_dir_all(&bins).unwrap();
    for b in ["kairos-driver", "kairos-core", "kairos-replayd", "kairos_sim_hubd"] {
        write_fake(&bins.join(b), &sentinel);
    }

    // Sim namespace under /tmp: distinct from live, so the isolation guard passes.
    // The fakes never create cnc.dat or the sockets, so `wait_ready` blocks (no
    // handler installed yet) — exactly the window we are probing.
    let aeron = base.join("aeron");
    let sim = env!("CARGO_BIN_EXE_kairos-sim");
    let mut child = Command::new(sim)
        .arg("up")
        .args(["--symbols", "2330"])
        .arg("--bin-dir")
        .arg(&bins)
        .arg("--hubd")
        .arg(bins.join("kairos_sim_hubd"))
        .env("KAIROS_SIM_AERON_DIR", &aeron)
        .env("KAIROS_SIM_QUOTE_SOCK", base.join("q.sock"))
        .env("KAIROS_SIM_ORDER_SOCK", base.join("o.sock"))
        .spawn()
        .expect("spawn kairos-sim");

    // Wait until the three children are up (still inside wait_ready).
    let deadline = Instant::now() + Duration::from_secs(5);
    while count_sentinel(&sentinel) < 3 && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(50));
    }
    assert_eq!(count_sentinel(&sentinel), 3, "sim children never came up");

    // SIGTERM kairos-sim in the pre-handler window.
    let _ = Command::new("kill")
        .args(["-TERM", &child.id().to_string()])
        .status();
    let deadline = Instant::now() + Duration::from_secs(10);
    while Instant::now() < deadline {
        if matches!(child.try_wait(), Ok(Some(_))) {
            break;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    std::thread::sleep(Duration::from_millis(500));

    let orphans = count_sentinel(&sentinel);
    let pidfile = std::path::PathBuf::from(format!("{}.kairos-sim.pids", aeron.display()));
    let pidfile_present = pidfile.exists();

    // Cleanup before asserting.
    let _ = child.kill();
    let _ = child.wait();
    let _ = Command::new("pkill")
        .args(["-KILL", "-f", &format!("sleep {sentinel}")])
        .status();
    let _ = std::fs::remove_dir_all(&base);

    // The invariant the task requires:
    assert_eq!(
        orphans, 0,
        "ORPHANS: {orphans} sim children survived SIGTERM during startup (pidfile_present={pidfile_present})"
    );
}
