//! Real TUI-source <-> supervisor-daemon E2E: bring up the ISOLATED kairos-sim
//! replaying the trend_day_2330 tape, run the S1 `kairos_scenario_supervisord`
//! against the sim sockets, and drive the REAL `kairos_tui::sources::supervisor`
//! client through list/start(test)/stop — proving the Rust client speaks the S1
//! wire format byte-for-byte, the state advances to a running state with fills,
//! and a stop returns it to stopped. Then tear everything down and assert no
//! orphan trader and no orphan sim process group.
//!
//! Ignored by default (spawns the sim daemons + the C++ sim hub + supervisor).
//! Run after building both sides:
//!   KAIROS_SIM_BIN=core/target/release/kairos-sim \
//!   KAIROS_SIM_HUBD=exec/scenario/build/kairos_sim_hubd \
//!   KAIROS_SCENARIO_TRADER=exec/scenario/build/kairos_scenario_trader \
//!   KAIROS_SCENARIO_SUPERVISORD=exec/scenario/build/kairos_scenario_supervisord \
//!     cargo test -p kairos-tui --test supervisor_e2e -- --ignored --nocapture

use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, Instant};

use kairos_tui::sources::supervisor::{self, ScenarioState, SupervisorState};

fn manifest() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

fn root() -> PathBuf {
    manifest().parent().unwrap().to_path_buf()
}

fn locate(env_var: &str, rel: &str) -> PathBuf {
    if let Ok(p) = std::env::var(env_var)
        && !p.is_empty()
    {
        return PathBuf::from(p);
    }
    for base in [root(), PathBuf::from("/home/coder/Kairos")] {
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

fn wait_pid_gone(pid: i32, timeout: Duration) -> bool {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        // SAFETY: kill(pid, 0) probes liveness only; ESRCH means the pid is gone.
        if unsafe { libc::kill(pid, 0) } == -1 {
            return true;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    (unsafe { libc::kill(pid, 0) }) == -1
}

/// Wait for an owned child to exit, reaping it (a bare `kill(pid, 0)` would still
/// see a not-yet-waited zombie as alive). Returns true once it has exited.
fn wait_child_exit(child: &mut std::process::Child, timeout: Duration) -> bool {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if matches!(child.try_wait(), Ok(Some(_))) {
            return true;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    matches!(child.try_wait(), Ok(Some(_)))
}

fn read_pgids(pidfile: &Path) -> Vec<i32> {
    std::fs::read_to_string(pidfile)
        .unwrap_or_default()
        .lines()
        .filter_map(|l| l.split_once(' ').and_then(|(p, _)| p.trim().parse().ok()))
        .collect()
}

/// Kills+reaps the sim and the supervisor on drop, so any panic cannot orphan the
/// pipeline. SIGTERM the supervisor (it SIGINT-reaps its traders) and the sim group
/// (its guard tears down its child groups), then force-kill any recorded sim pgid.
struct Reaper {
    sim: std::process::Child,
    sup: std::process::Child,
    pidfile: PathBuf,
    cleanup: Vec<PathBuf>,
}

impl Drop for Reaper {
    fn drop(&mut self) {
        if matches!(self.sup.try_wait(), Ok(None)) {
            unsafe { libc::kill(self.sup.id() as i32, libc::SIGTERM) };
            let _ = self.sup.wait();
        }
        if matches!(self.sim.try_wait(), Ok(None)) {
            // The sim owns its own process group (setpgid in pre_exec).
            unsafe { libc::killpg(self.sim.id() as i32, libc::SIGTERM) };
            let deadline = Instant::now() + Duration::from_secs(5);
            while Instant::now() < deadline {
                if matches!(self.sim.try_wait(), Ok(Some(_))) {
                    break;
                }
                std::thread::sleep(Duration::from_millis(50));
            }
            for pgid in read_pgids(&self.pidfile) {
                unsafe { libc::killpg(pgid, libc::SIGKILL) };
            }
            let _ = self.sim.kill();
            let _ = self.sim.wait();
        }
        for dir in &self.cleanup {
            let _ = std::fs::remove_dir_all(dir);
        }
    }
}

#[test]
#[ignore = "spawns the sim + C++ sim hub + supervisor; run with --ignored"]
fn tui_supervisor_source_drives_a_real_daemon() {
    let tag = std::process::id();
    let base = std::env::temp_dir().join(format!("kairos-sup-e2e-{tag}"));
    let _ = std::fs::remove_dir_all(&base);
    std::fs::create_dir_all(&base).unwrap();

    let aeron_dir = format!("/dev/shm/aeron-sup-e2e-{tag}");
    let quote_sock = base.join("q.sock");
    let order_sock = base.join("o.sock");
    let ctl_sock = base.join("ctl.sock");
    let scenario_dir = base.join("scn");
    std::fs::create_dir_all(&scenario_dir).unwrap();
    std::fs::write(
        scenario_dir.join("2330.toml"),
        "[scenario]\nname=\"2330\"\nsymbol=\"2330\"\nside=\"Buy\"\nbudget_twd=300000\n\
         board=\"OddLot\"\n[pricing]\npolicy=\"cross\"\n[mode]\nlive=false\n",
    )
    .unwrap();
    let pidfile = PathBuf::from(format!("{aeron_dir}.kairos-sim.pids"));

    let sim_bin = locate("KAIROS_SIM_BIN", "core/target/release/kairos-sim");
    let bin_dir = sim_bin.parent().unwrap().to_path_buf();
    let hubd = locate("KAIROS_SIM_HUBD", "exec/scenario/build/kairos_sim_hubd");
    let trader = locate(
        "KAIROS_SCENARIO_TRADER",
        "exec/scenario/build/kairos_scenario_trader",
    );
    let supervisord = locate(
        "KAIROS_SCENARIO_SUPERVISORD",
        "exec/scenario/build/kairos_scenario_supervisord",
    );
    let tape = root().join("core/tests/fixtures/tapes/trend_day_2330.kqr");
    assert!(tape.is_file(), "trend_day tape missing: {}", tape.display());

    // 1) Bring up the isolated sim replaying the fixture tape, in its own group.
    let mut sim_cmd = Command::new(&sim_bin);
    sim_cmd
        .arg("replay")
        .arg(&tape)
        .args(["--symbols", "2330", "--speed", "8"])
        .arg("--quote-sock")
        .arg(&quote_sock)
        .arg("--order-sock")
        .arg(&order_sock)
        .arg("--aeron-dir")
        .arg(&aeron_dir)
        .arg("--hubd")
        .arg(&hubd)
        .arg("--bin-dir")
        .arg(&bin_dir);
    unsafe {
        sim_cmd.pre_exec(|| {
            // Own process group so the reaper can killpg the whole sim pipeline.
            if libc::setpgid(0, 0) == -1 {
                return Err(std::io::Error::last_os_error());
            }
            Ok(())
        });
    }
    let sim = sim_cmd.spawn().expect("spawn kairos-sim");

    // 2) Launch the supervisor pointed at the sim sockets (traders inherit them).
    let sup = Command::new(&supervisord)
        .arg("--scenario-dir")
        .arg(&scenario_dir)
        .arg("--trader-bin")
        .arg(&trader)
        .arg("--ctl-sock")
        .arg(&ctl_sock)
        .env("KAIROS_QUOTE_SOCK", &quote_sock)
        .env("KAIROS_ORDER_SOCK", &order_sock)
        .spawn()
        .expect("spawn kairos_scenario_supervisord");

    let mut guard = Reaper {
        sim,
        sup,
        pidfile: pidfile.clone(),
        cleanup: vec![base.clone(), PathBuf::from(&aeron_dir)],
    };

    assert!(
        wait_for(&quote_sock, Duration::from_secs(20)),
        "sim quote socket never appeared"
    );
    assert!(
        wait_for(&ctl_sock, Duration::from_secs(10)),
        "supervisor ctl socket never appeared"
    );
    println!("supervisor_e2e: sim + supervisor up");

    let rt = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap();
    let ctl = ctl_sock.clone();

    let trader_pid = rt.block_on(async move {
        // 3) list: the 2330 scenario is visible and stopped.
        let listed: SupervisorState = supervisor::poll_once(&ctl).await;
        assert!(listed.connected, "client could not reach the daemon");
        assert!(
            listed.rows.iter().any(|r| r.name == "2330"),
            "2330 not in the snapshot: {:?}",
            listed.rows
        );
        println!("supervisor_e2e: list ok, {} scenario(s)", listed.rows.len());

        // 4) start 2330 in TEST mode (stays alive off-hours via --ignore-window).
        let start = supervisor::send_command(
            &ctl,
            &supervisor::start_request("2330", supervisor::Mode::Test),
        )
        .await;
        println!("supervisor_e2e: start -> {start}");
        assert_eq!(start, "ok", "start command was rejected");

        // 5) poll list until the state advances to a running state with fills.
        let mut trader_pid = 0i64;
        let mut advanced = false;
        let deadline = Instant::now() + Duration::from_secs(40);
        while Instant::now() < deadline {
            let st = supervisor::poll_once(&ctl).await;
            if let Some(r) = st.rows.iter().find(|r| r.name == "2330") {
                if r.pid > 0 {
                    trader_pid = r.pid;
                }
                println!(
                    "supervisor_e2e: state={} pid={} cum_fills={} cum_shares={}",
                    r.state.name(),
                    r.pid,
                    r.cum_fills,
                    r.cum_shares
                );
                let running = matches!(
                    r.state,
                    ScenarioState::InWindow | ScenarioState::FillRemainder
                );
                if running && r.cum_fills > 0 {
                    advanced = true;
                    break;
                }
            }
            tokio::time::sleep(Duration::from_millis(500)).await;
        }
        assert!(
            advanced,
            "state never advanced to a running state with fills"
        );
        assert!(trader_pid > 0, "no trader pid was ever reported");
        println!("supervisor_e2e: ADVANCED, trader pid={trader_pid}");

        // 6) stop 2330 -> it returns to a stopped/exited state.
        let stop = supervisor::send_command(&ctl, &supervisor::stop_request("2330")).await;
        println!("supervisor_e2e: stop -> {stop}");
        assert_eq!(stop, "ok", "stop command was rejected");

        let mut stopped = false;
        let deadline = Instant::now() + Duration::from_secs(10);
        while Instant::now() < deadline {
            let st = supervisor::poll_once(&ctl).await;
            if let Some(r) = st.rows.iter().find(|r| r.name == "2330")
                && !r.state.is_running()
            {
                println!("supervisor_e2e: stopped state={}", r.state.name());
                stopped = true;
                break;
            }
            tokio::time::sleep(Duration::from_millis(200)).await;
        }
        assert!(
            stopped,
            "2330 never returned to a non-running state after stop"
        );

        trader_pid
    });

    // 7) The stopped trader must be reaped by the daemon (no orphan child).
    assert!(
        wait_pid_gone(trader_pid as i32, Duration::from_secs(6)),
        "trader pid {trader_pid} still alive after stop"
    );
    println!("supervisor_e2e: trader pid {trader_pid} reaped (ESRCH)");

    // 8) Tear the supervisor down (must reap any live trader) and the sim group.
    let sup_pid = guard.sup.id() as i32;
    unsafe { libc::kill(sup_pid, libc::SIGTERM) };
    assert!(
        wait_child_exit(&mut guard.sup, Duration::from_secs(8)),
        "supervisor did not exit after SIGTERM"
    );
    println!("supervisor_e2e: supervisor exited");

    assert!(
        wait_for(&pidfile, Duration::from_secs(5)),
        "sim pidfile never written"
    );
    let pgids = read_pgids(&pidfile);
    assert!(!pgids.is_empty(), "no sim child pgids recorded");

    let sim_pid = guard.sim.id() as i32;
    unsafe { libc::killpg(sim_pid, libc::SIGTERM) };
    assert!(
        wait_child_exit(&mut guard.sim, Duration::from_secs(10)),
        "kairos-sim did not exit after SIGTERM"
    );

    std::thread::sleep(Duration::from_millis(400));
    for pgid in &pgids {
        // killpg(pgid, 0) probes liveness only; -1 (ESRCH) means the group is gone.
        let rc = unsafe { libc::killpg(*pgid, 0) };
        assert_eq!(rc, -1, "orphan sim process group {pgid} still alive");
    }
    println!("supervisor_e2e: teardown clean, no orphan process groups");
}
