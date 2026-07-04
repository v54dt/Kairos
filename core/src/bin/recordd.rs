//! kairos-recordd — archives raw Aeron fragments (streams 1001 data + 1002
//! control) to dated KQR files for replay/backtest. Independent of the trading
//! path: a crash or slow disk here never affects core or the live feed.
//!
//! Usage: kairos-recordd [OUT_DIR] [--aeron-dir DIR] [--streams 1001,1002]
//!
//! Refuses to start if a `kairos-replay.marker` for a live replay is present in
//! the effective Aeron dir, so a replay can never pollute the real archive.

use mimalloc::MiMalloc;

#[global_allocator]
static GLOBAL: MiMalloc = MiMalloc;

use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::{Duration, Instant};

use kairos_core::ipc::aeron::resolve_aeron_dir;
use kairos_core::record::recorder::{Stats, run_stream};
use kairos_core::replay::{default_aeron_dir, ensure_no_active_replay};

struct Args {
    out_dir: PathBuf,
    aeron_dir: Option<String>,
    streams: Vec<i32>,
}

fn parse_args() -> anyhow::Result<Args> {
    let mut out_dir: Option<PathBuf> = None;
    let mut aeron_dir: Option<String> = None;
    let mut streams: Vec<i32> = vec![1001, 1002];
    let mut it = std::env::args().skip(1);
    while let Some(a) = it.next() {
        match a.as_str() {
            "--aeron-dir" => {
                aeron_dir = Some(
                    it.next()
                        .ok_or_else(|| anyhow::anyhow!("--aeron-dir needs a value"))?,
                )
            }
            "--streams" => {
                let v = it
                    .next()
                    .ok_or_else(|| anyhow::anyhow!("--streams needs a value"))?;
                streams = v
                    .split(',')
                    .map(|s| s.trim().parse::<i32>())
                    .collect::<Result<_, _>>()?;
            }
            _ if a.starts_with("--") => anyhow::bail!("unknown flag {a}"),
            _ => out_dir = Some(PathBuf::from(a)),
        }
    }
    Ok(Args {
        out_dir: out_dir.unwrap_or_else(|| PathBuf::from("data")),
        aeron_dir,
        streams,
    })
}

fn stats_loop(streams: Vec<i32>, stats: Vec<Arc<Stats>>, stop: Arc<AtomicBool>) {
    while !stop.load(Ordering::Relaxed) {
        let until = Instant::now() + Duration::from_secs(10);
        while Instant::now() < until && !stop.load(Ordering::Relaxed) {
            thread::sleep(Duration::from_millis(200));
        }
        for (sid, s) in streams.iter().zip(&stats) {
            eprintln!(
                "kairos-recordd: stream {sid} records={} bytes={} drops={} write_errs={} disk_free_mib={}",
                s.records.load(Ordering::Relaxed),
                s.bytes.load(Ordering::Relaxed),
                s.drops.load(Ordering::Relaxed),
                s.write_errs.load(Ordering::Relaxed),
                s.disk_free.load(Ordering::Relaxed) / (1024 * 1024),
            );
        }
    }
}

fn main() -> anyhow::Result<()> {
    let args = parse_args()?;
    // Same resolution order as the aeron client: --aeron-dir, else $KAIROS_AERON_DIR,
    // else the live default. Refuse if a replay owns that dir.
    if let Some(dir) = resolve_aeron_dir(args.aeron_dir.as_deref()).or_else(default_aeron_dir) {
        ensure_no_active_replay(&dir)?;
    }
    let stop = Arc::new(AtomicBool::new(false));
    ctrlc::set_handler({
        let stop = stop.clone();
        move || stop.store(true, Ordering::SeqCst)
    })?;

    let stats: Vec<Arc<Stats>> = args
        .streams
        .iter()
        .map(|_| Arc::new(Stats::default()))
        .collect();
    eprintln!(
        "kairos-recordd: recording streams {:?} -> {}",
        args.streams,
        args.out_dir.display()
    );

    thread::spawn({
        let (streams, stats, stop) = (args.streams.clone(), stats.clone(), stop.clone());
        move || stats_loop(streams, stats, stop)
    });

    let mut handles = Vec::new();
    for (i, &sid) in args.streams.iter().enumerate() {
        let (dir, out, stop, st) = (
            args.aeron_dir.clone(),
            args.out_dir.clone(),
            stop.clone(),
            stats[i].clone(),
        );
        handles.push(thread::spawn(move || run_stream(sid, dir, out, stop, st)));
    }
    for h in handles {
        let _ = h.join();
    }
    eprintln!("kairos-recordd: shutting down");
    Ok(())
}
