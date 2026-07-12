//! kairos-replayd — replays recorded KQR fragments back onto an Aeron stream at
//! one of three paces (realtime / accel / full-speed). Reader only: it re-offers
//! raw envelope bytes and never decodes a payload.
//!
//! Isolation: `--aeron-dir` is REQUIRED (no default) and the live stack's Aeron dir
//! (`$KAIROS_AERON_DIR`, else `$AERON_DIR`, else `/dev/shm/aeron-<user>`) is refused
//! unless `--force-live-dir` is given, so a fat-fingered replay can never land on the
//! live feed. While running it writes `kairos-replay.marker`
//! into that dir; kairos-recordd refuses to start there (防污染正本 archive).
//!
//! Usage:
//!   kairos-replayd FILE... --aeron-dir DIR [--pace PACE] [--speed F] [--stream-remap A:B]
//!   kairos-replayd --date YYYYMMDD --streams 1001,1002 [--data-dir DIR] --aeron-dir DIR ...
//! PACE = realtime | accel | full  (accel uses --speed, default 2.0; any factor >0)

use mimalloc::MiMalloc;

#[global_allocator]
static GLOBAL: MiMalloc = MiMalloc;

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::thread;
use std::time::{Duration, Instant};

use kairos_core::daemon::{install_stop_flag, stats_loop};
use kairos_core::ipc::aeron::AeronPub;
use kairos_core::replay::{
    KqrSource, OfferOutcome, Pace, Pacer, ReplayRecord, ReplayStats, SystemClock, drive_replay,
    effective_stack_dir, refuses_live_dir, write_marker,
};

/// Extra offer attempts on top of `AeronPub::offer`'s own retries before a record
/// is dropped, so a dead subscriber costs a bounded delay instead of an endless spin.
const OUTER_OFFER_RETRIES: usize = 3;
const OFFER_BACKOFF: Duration = Duration::from_millis(2);
const WARN_INTERVAL: Duration = Duration::from_secs(5);

struct Args {
    files: Vec<PathBuf>,
    date: Option<String>,
    streams: Vec<u32>,
    data_dir: PathBuf,
    aeron_dir: Option<String>,
    pace: Pace,
    remap: HashMap<u32, u32>,
    force_live_dir: bool,
}

fn parse_remap(spec: &str, remap: &mut HashMap<u32, u32>) -> anyhow::Result<()> {
    for pair in spec.split(',').filter(|s| !s.is_empty()) {
        let (a, b) = pair
            .split_once(':')
            .ok_or_else(|| anyhow::anyhow!("--stream-remap expects A:B, got {pair}"))?;
        remap.insert(a.trim().parse::<u32>()?, b.trim().parse::<u32>()?);
    }
    Ok(())
}

fn parse_args() -> anyhow::Result<Args> {
    let mut files = Vec::new();
    let mut date: Option<String> = None;
    let mut streams: Vec<u32> = Vec::new();
    let mut data_dir = PathBuf::from("data");
    let mut aeron_dir: Option<String> = None;
    let mut pace_str = String::from("realtime");
    let mut speed = 2.0f64;
    let mut remap = HashMap::new();
    let mut force_live_dir = false;

    let mut it = std::env::args().skip(1);
    while let Some(a) = it.next() {
        let mut next = || {
            it.next()
                .ok_or_else(|| anyhow::anyhow!("{a} needs a value"))
        };
        match a.as_str() {
            "--aeron-dir" => aeron_dir = Some(next()?),
            "--date" => date = Some(next()?),
            "--data-dir" => data_dir = PathBuf::from(next()?),
            "--pace" => pace_str = next()?,
            "--speed" => speed = next()?.parse::<f64>()?,
            "--streams" => {
                streams = next()?
                    .split(',')
                    .filter(|s| !s.is_empty())
                    .map(|s| s.trim().parse::<u32>())
                    .collect::<Result<_, _>>()?;
            }
            "--stream-remap" => parse_remap(&next()?, &mut remap)?,
            "--force-live-dir" => force_live_dir = true,
            _ if a.starts_with("--") => anyhow::bail!("unknown flag {a}"),
            _ => files.push(PathBuf::from(a)),
        }
    }

    if speed <= 0.0 || !speed.is_finite() {
        anyhow::bail!("--speed must be a positive factor, got {speed}");
    }
    let pace = match pace_str.as_str() {
        "realtime" => Pace::Realtime,
        "accel" => Pace::Accel(speed),
        "full" => Pace::Full,
        other => anyhow::bail!("--pace must be realtime|accel|full, got {other}"),
    };

    Ok(Args {
        files,
        date,
        streams,
        data_dir,
        aeron_dir,
        pace,
        remap,
        force_live_dir,
    })
}

/// Open the playback source from either an explicit file list or a date+streams
/// selector under `--data-dir` (recordd's naming convention).
fn open_source(args: &Args) -> anyhow::Result<KqrSource> {
    if !args.files.is_empty() {
        return Ok(KqrSource::open_files(&args.files)?);
    }
    match &args.date {
        Some(date) if !args.streams.is_empty() => Ok(KqrSource::open_selector(
            &args.data_dir,
            date,
            &args.streams,
        )?),
        _ => anyhow::bail!("give KQR files, or --date YYYYMMDD with --streams"),
    }
}

fn render_stats(stats: &ReplayStats, remap: &HashMap<u32, u32>, start: Instant) -> String {
    let replayed_us = stats
        .replay_span_us()
        .map(|(a, b)| b - a)
        .unwrap_or(0)
        .max(0);
    let wall_us = start.elapsed().as_micros() as i64;
    let ratio = if wall_us > 0 {
        replayed_us as f64 / wall_us as f64
    } else {
        0.0
    };
    let per_stream: Vec<String> = stats
        .per_stream()
        .iter()
        .map(|(s, c)| {
            let out = remap.get(s).copied().unwrap_or(*s);
            format!("{out}={c}")
        })
        .collect();
    format!(
        "kairos-replayd: offered={} dropped={} bytes={} replayed={}ms wall={}ms ratio={ratio:.2}x streams[{}]",
        stats.offered.load(Ordering::Relaxed),
        stats.dropped.load(Ordering::Relaxed),
        stats.bytes.load(Ordering::Relaxed),
        replayed_us / 1000,
        wall_us / 1000,
        per_stream.join(" "),
    )
}

fn main() -> anyhow::Result<()> {
    let args = parse_args()?;

    let aeron_dir = args
        .aeron_dir
        .clone()
        .ok_or_else(|| anyhow::anyhow!("--aeron-dir is required (no default, by design)"))?;
    if refuses_live_dir(
        &aeron_dir,
        effective_stack_dir(None).as_deref(),
        args.force_live_dir,
    ) {
        anyhow::bail!(
            "{aeron_dir} is the live stack's Aeron dir; refusing to replay onto the live feed. \
             Use a separate --aeron-dir, or --force-live-dir to override"
        );
    }

    let source = open_source(&args)?;
    let src_streams = source.stream_ids();
    let out_streams: Vec<u32> = {
        let mut v: Vec<u32> = src_streams
            .iter()
            .map(|s| args.remap.get(s).copied().unwrap_or(*s))
            .collect();
        v.sort_unstable();
        v.dedup();
        v
    };
    if out_streams.is_empty() {
        anyhow::bail!("no records to replay (empty input)");
    }

    let mut pubs: HashMap<u32, AeronPub> = HashMap::new();
    for &s in &out_streams {
        let publication = AeronPub::connect(Some(&aeron_dir), s as i32)
            .map_err(|e| anyhow::anyhow!("connect publication stream {s}: {e:?}"))?;
        pubs.insert(s, publication);
    }

    let stop = install_stop_flag()?;

    let _marker = write_marker(&aeron_dir)?;
    eprintln!(
        "kairos-replayd: replaying {:?} -> aeron_dir={aeron_dir} pace={:?}",
        out_streams, args.pace
    );

    let stats = Arc::new(ReplayStats::new(&src_streams));
    let start = Instant::now();
    let stats_thread = thread::spawn({
        let (stats, stop) = (stats.clone(), stop.clone());
        let remap = args.remap.clone();
        move || stats_loop(&stop, || render_stats(&stats, &remap, start))
    });

    let remap = args.remap.clone();
    let mut last_warn: Option<Instant> = None;
    let sink = |rec: &ReplayRecord| -> OfferOutcome {
        let out = remap.get(&rec.stream_id).copied().unwrap_or(rec.stream_id);
        let Some(publication) = pubs.get(&out) else {
            return OfferOutcome::Dropped;
        };
        for attempt in 0..=OUTER_OFFER_RETRIES {
            if publication.offer(&rec.payload).is_ok() {
                return OfferOutcome::Offered;
            }
            if stop.load(Ordering::Relaxed) || attempt == OUTER_OFFER_RETRIES {
                break;
            }
            thread::sleep(OFFER_BACKOFF);
        }
        if last_warn.is_none_or(|t| t.elapsed() >= WARN_INTERVAL) {
            eprintln!("kairos-replayd: backpressure, dropping records on stream {out}");
            last_warn = Some(Instant::now());
        }
        OfferOutcome::Dropped
    };

    let clock = SystemClock::new(Some(stop.clone()));
    let pacer = Pacer::new(source, clock, args.pace);
    drive_replay(pacer, &stop, &stats, sink);

    stop.store(true, Ordering::SeqCst);
    let _ = stats_thread.join();
    eprintln!(
        "kairos-replayd: done — offered={} dropped={} bytes={}",
        stats.offered.load(Ordering::Relaxed),
        stats.dropped.load(Ordering::Relaxed),
        stats.bytes.load(Ordering::Relaxed),
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    // The wall integer is the only timing-dependent field; the rest of the line is
    // pinned byte-for-byte so the user-facing stats format cannot drift.
    #[test]
    fn render_stats_format_is_byte_identical() {
        let stats = ReplayStats::new(&[1001]);
        stats.offered.store(5, Ordering::Relaxed);
        stats.dropped.store(1, Ordering::Relaxed);
        stats.bytes.store(100, Ordering::Relaxed);
        let line = render_stats(&stats, &HashMap::new(), Instant::now());
        assert!(
            line.starts_with("kairos-replayd: offered=5 dropped=1 bytes=100 replayed=0ms wall="),
            "{line}"
        );
        assert!(line.ends_with("ms ratio=0.00x streams[1001=0]"), "{line}");
    }
}
