//! kairos-sim — one command to bring up an ISOLATED sim universe (Aeron dir +
//! quote/order sockets) that a trader / kairos-uds-client / kairos-top can attach
//! to, without ever touching the live pipeline.
//!
//! The sim namespace (`$KAIROS_SIM_AERON_DIR`, `$KAIROS_SIM_QUOTE_SOCK`,
//! `$KAIROS_SIM_ORDER_SOCK`, each with a `-sim` default) is distinct from live by
//! construction, and a hard isolation guard runs BEFORE any spawn: it refuses to
//! start if any resolved sim path canonicalizes onto the live Aeron dir or either
//! live socket. Every child is spawned into its own process group and killed on
//! exit / Ctrl-C / SIGTERM / panic, so no sim daemon is ever orphaned.
//!
//! Usage:
//!   kairos-sim up --symbols A,B
//!   kairos-sim replay <FILE|DIR> --symbols A,B [--speed N]
//!   kairos-sim down
//!   kairos-sim status

use mimalloc::MiMalloc;

#[global_allocator]
static GLOBAL: MiMalloc = MiMalloc;

use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use kairos_core::replay::marker::effective_stack_dir;
use kairos_core::sim::cli::{self, Opts};
use kairos_core::sim::paths::SimPaths;
use kairos_core::sim::proc::{
    self, ChildGuard, Spawned, locate_bin, read_pidfile, resolve_hubd, sibling_bin_dir, wait_ready,
    write_pidfile,
};
use kairos_core::sim::{Command as SimCommand, ensure_isolated};
use kairos_core::uds::path::{order_socket_path, quote_socket_path};

const READY_TIMEOUT: Duration = Duration::from_secs(15);
const KILL_GRACE: Duration = Duration::from_secs(3);

fn main() -> anyhow::Result<()> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let cmd = cli::parse(&args)?;

    match cmd {
        SimCommand::Up(opts) => run(&opts, None)?,
        SimCommand::Replay {
            source,
            speed,
            opts,
        } => run(&opts, Some((source, speed)))?,
        SimCommand::Down(opts) => down(&opts)?,
        SimCommand::Status(opts) => status(&opts)?,
    }
    Ok(())
}

fn env_opt(key: &str) -> Option<String> {
    std::env::var(key).ok().filter(|s| !s.is_empty())
}

/// Resolve the sim namespace and enforce isolation against the live pipeline. A
/// `--flag` override wins over the matching `KAIROS_SIM_*` env, which wins over the
/// namespaced default.
fn resolve_isolated(opts: &Opts) -> anyhow::Result<SimPaths> {
    let aeron = opts
        .aeron_dir
        .clone()
        .or_else(|| env_opt("KAIROS_SIM_AERON_DIR"));
    let quote = opts
        .quote_sock
        .clone()
        .or_else(|| env_opt("KAIROS_SIM_QUOTE_SOCK"));
    let order = opts
        .order_sock
        .clone()
        .or_else(|| env_opt("KAIROS_SIM_ORDER_SOCK"));
    let paths = SimPaths::resolve_with(
        aeron.as_deref(),
        quote.as_deref(),
        order.as_deref(),
        kairos_core::uds::path::runtime_dir().as_deref(),
    )?;
    ensure_isolated(
        &paths,
        effective_stack_dir(None).as_deref(),
        &quote_socket_path(),
        &order_socket_path(),
    )?;
    Ok(paths)
}

/// Bring up the isolated pipeline and (optionally) a replay, then block until a
/// signal and tear everything down. `replay` is Some((source, speed)) for the
/// replay subcommand, None for `up`.
fn run(opts: &Opts, replay: Option<(PathBuf, Option<f64>)>) -> anyhow::Result<()> {
    let paths = resolve_isolated(opts)?;
    let exe = std::env::current_exe()?;
    let bin_dir = match &opts.bin_dir {
        Some(d) => PathBuf::from(d),
        None => sibling_bin_dir(&exe)?,
    };
    let driver_bin = locate_bin(&bin_dir, "kairos-driver")?;
    let core_bin = locate_bin(&bin_dir, "kairos-core")?;
    let hubd_override = opts.hubd.clone().or_else(|| {
        std::env::var("KAIROS_SIM_HUBD")
            .ok()
            .filter(|s| !s.is_empty())
    });
    let hubd_bin = resolve_hubd(hubd_override.as_deref(), &exe)?;
    let replayd_bin = match &replay {
        Some(_) => Some(locate_bin(&bin_dir, "kairos-replayd")?),
        None => None,
    };

    let mut guard = ChildGuard::new();

    let mut driver = Command::new(&driver_bin);
    driver.env("KAIROS_AERON_DIR", &paths.aeron_dir);
    guard.push(Spawned::start("kairos-driver", driver)?);

    let mut core = Command::new(&core_bin);
    core.env("KAIROS_AERON_DIR", &paths.aeron_dir)
        .env("KAIROS_QUOTE_SOCK", &paths.quote_sock);
    guard.push(Spawned::start("kairos-core", core)?);

    let mut hubd = Command::new(&hubd_bin);
    hubd.args(&opts.symbols)
        .arg("--order-sock")
        .arg(&paths.order_sock)
        .arg("--quote-sock")
        .arg(&paths.quote_sock);
    if opts.prob {
        hubd.arg("--prob");
    }
    guard.push(Spawned::start("kairos_sim_hubd", hubd)?);

    wait_ready(&paths, &mut guard, READY_TIMEOUT)?;

    if let (Some(bin), Some((source, speed))) = (&replayd_bin, &replay) {
        // No KAIROS_AERON_DIR override: replayd publishes via --aeron-dir=sim, and its
        // own live-dir guard must keep seeing the real live dir from the inherited env.
        let mut replayd = Command::new(bin);
        replayd.arg(source).arg("--aeron-dir").arg(&paths.aeron_dir);
        if let Some(n) = speed {
            replayd
                .args(["--pace", "accel", "--speed"])
                .arg(n.to_string());
        }
        guard.push(Spawned::start("kairos-replayd", replayd)?);
    }

    write_pidfile(&paths, &guard.pgids())?;
    print_ready(&paths, replay.is_some());

    let stop = Arc::new(AtomicBool::new(false));
    ctrlc::set_handler({
        let stop = stop.clone();
        move || stop.store(true, Ordering::SeqCst)
    })?;

    while !stop.load(Ordering::Relaxed) {
        let dead = guard.exited();
        if !dead.is_empty() {
            eprintln!("kairos-sim: child exited unexpectedly: {}", dead.join(", "));
            break;
        }
        std::thread::sleep(Duration::from_millis(200));
    }

    eprintln!("kairos-sim: tearing down the sim pipeline");
    drop(guard);
    proc::remove_pidfile(&paths);
    Ok(())
}

fn print_ready(paths: &SimPaths, replaying: bool) {
    let what = if replaying { "replay" } else { "sim" };
    println!("kairos-sim: isolated {what} pipeline is up. Attach with:");
    println!("  export KAIROS_AERON_DIR={}", paths.aeron_dir);
    println!("  export KAIROS_QUOTE_SOCK={}", paths.quote_sock);
    println!("  export KAIROS_ORDER_SOCK={}", paths.order_sock);
    println!("kairos-sim: Ctrl-C to tear everything down.");
}

/// Idempotently kill a running sim's process groups (from the pidfile). Returns 0
/// even when nothing is running.
fn down(opts: &Opts) -> anyhow::Result<()> {
    let paths = resolve_isolated(opts)?;
    let entries = read_pidfile(&paths);
    if entries.is_empty() {
        println!("kairos-sim: no running sim found (nothing to do)");
        return Ok(());
    }
    for (pgid, name) in &entries {
        // SAFETY: killpg with a valid pgid signals only that group.
        unsafe { libc::killpg(*pgid, libc::SIGTERM) };
        println!("kairos-sim: SIGTERM {name} (pgid {pgid})");
    }
    let deadline = std::time::Instant::now() + KILL_GRACE;
    while std::time::Instant::now() < deadline && entries.iter().any(|(p, _)| proc::pgid_alive(*p))
    {
        std::thread::sleep(Duration::from_millis(50));
    }
    for (pgid, name) in &entries {
        if proc::pgid_alive(*pgid) {
            // SAFETY: same as above; force-kill a group that ignored SIGTERM.
            unsafe { libc::killpg(*pgid, libc::SIGKILL) };
            println!("kairos-sim: SIGKILL {name} (pgid {pgid})");
        }
    }
    proc::remove_pidfile(&paths);
    Ok(())
}

fn status(opts: &Opts) -> anyhow::Result<()> {
    let paths = resolve_isolated(opts)?;
    println!("kairos-sim namespace:");
    println!("  KAIROS_AERON_DIR={}", paths.aeron_dir);
    println!("  KAIROS_QUOTE_SOCK={}", paths.quote_sock);
    println!("  KAIROS_ORDER_SOCK={}", paths.order_sock);
    let cnc = Path::new(&paths.aeron_dir).join(proc::CNC_FILE);
    println!("  driver cnc.dat: {}", present(cnc.exists()));
    println!(
        "  quote socket:   {}",
        present(Path::new(&paths.quote_sock).exists())
    );
    println!(
        "  order socket:   {}",
        present(Path::new(&paths.order_sock).exists())
    );
    let entries = read_pidfile(&paths);
    if entries.is_empty() {
        println!("  processes: none recorded (sim is down)");
    } else {
        for (pgid, name) in entries {
            println!(
                "  {name}: pgid {pgid} {}",
                if proc::pgid_alive(pgid) {
                    "running"
                } else {
                    "dead"
                }
            );
        }
    }
    Ok(())
}

fn present(b: bool) -> &'static str {
    if b { "present" } else { "absent" }
}
