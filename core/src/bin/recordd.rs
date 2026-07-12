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
use std::sync::atomic::Ordering;
use std::thread;

use kairos_core::daemon::{install_stop_flag, stats_loop};
use kairos_core::record::recorder::{Stats, run_stream};
use kairos_core::replay::{effective_stack_dir, ensure_no_active_replay};

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

/// One stderr block per stats cycle: the per-stream lines joined by '\n', so the
/// bytes are identical to the previous one-eprintln-per-stream loop.
fn render_stats(streams: &[i32], stats: &[Arc<Stats>]) -> String {
    streams
        .iter()
        .zip(stats)
        .map(|(sid, s)| {
            format!(
                "kairos-recordd: stream {sid} records={} bytes={} drops={} write_errs={} disk_free_mib={}",
                s.records.load(Ordering::Relaxed),
                s.bytes.load(Ordering::Relaxed),
                s.drops.load(Ordering::Relaxed),
                s.write_errs.load(Ordering::Relaxed),
                s.disk_free.load(Ordering::Relaxed) / (1024 * 1024),
            )
        })
        .collect::<Vec<_>>()
        .join("\n")
}

fn main() -> anyhow::Result<()> {
    let args = parse_args()?;
    // Refuse if a replay owns the dir this recorder will actually subscribe from.
    if let Some(dir) = effective_stack_dir(args.aeron_dir.as_deref()) {
        ensure_no_active_replay(&dir)?;
    }
    let stop = install_stop_flag()?;

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
        move || stats_loop(&stop, || render_stats(&streams, &stats))
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn render_stats_is_byte_identical() {
        let stats = vec![Arc::new(Stats::default()), Arc::new(Stats::default())];
        stats[0].records.store(3, Ordering::Relaxed);
        stats[0].bytes.store(4096, Ordering::Relaxed);
        stats[0].drops.store(1, Ordering::Relaxed);
        stats[0].write_errs.store(0, Ordering::Relaxed);
        stats[0].disk_free.store(5 * 1024 * 1024, Ordering::Relaxed);
        stats[1].records.store(7, Ordering::Relaxed);
        stats[1].bytes.store(8192, Ordering::Relaxed);
        assert_eq!(
            render_stats(&[1001, 1002], &stats),
            "kairos-recordd: stream 1001 records=3 bytes=4096 drops=1 write_errs=0 disk_free_mib=5\n\
             kairos-recordd: stream 1002 records=7 bytes=8192 drops=0 write_errs=0 disk_free_mib=0"
        );
    }
}
