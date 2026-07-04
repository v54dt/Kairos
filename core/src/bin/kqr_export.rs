//! kairos-kqr-export — one-way, re-runnable converter from raw KQR capnp logs into
//! research outputs: Parquet `quotes`/`trades` derived tables (kairos-lab) and
//! per-(symbol, day) hftbacktest `_v1` event arrays as `.npy` (I4). The raw KQR
//! archive is the source of truth; this tool only reads it and writes elsewhere.
//!
//! Usage:
//!   kairos-kqr-export FILE... --date YYYYMMDD --out DIR [--symbols A,B] [--force]
//!   kairos-kqr-export --date YYYYMMDD --streams 1001,1002 [--data-dir DIR] \
//!       --out DIR [--symbols A,B] [--force]
//!
//! `--date` labels the output day (required in both modes). `--out` is required and
//! must not be inside the input directory. Existing outputs are never overwritten
//! unless `--force`.

use mimalloc::MiMalloc;

#[global_allocator]
static GLOBAL: MiMalloc = MiMalloc;

use std::collections::HashSet;
use std::path::PathBuf;

use kairos_core::export::convert::{convert, existing_outputs, out_inside_input};
use kairos_core::replay::KqrSource;

struct Args {
    files: Vec<PathBuf>,
    date: Option<String>,
    streams: Vec<u32>,
    data_dir: PathBuf,
    out: Option<PathBuf>,
    symbols: Option<HashSet<String>>,
    force: bool,
}

fn parse_args() -> anyhow::Result<Args> {
    let mut files = Vec::new();
    let mut date: Option<String> = None;
    let mut streams: Vec<u32> = Vec::new();
    let mut data_dir = PathBuf::from("data");
    let mut out: Option<PathBuf> = None;
    let mut symbols: Option<HashSet<String>> = None;
    let mut force = false;

    let mut it = std::env::args().skip(1);
    while let Some(a) = it.next() {
        let mut next = || {
            it.next()
                .ok_or_else(|| anyhow::anyhow!("{a} needs a value"))
        };
        match a.as_str() {
            "--date" => date = Some(next()?),
            "--out" => out = Some(PathBuf::from(next()?)),
            "--data-dir" => data_dir = PathBuf::from(next()?),
            "--streams" => {
                streams = next()?
                    .split(',')
                    .filter(|s| !s.is_empty())
                    .map(|s| s.trim().parse::<u32>())
                    .collect::<Result<_, _>>()?;
            }
            "--symbols" => {
                symbols = Some(
                    next()?
                        .split(',')
                        .filter(|s| !s.is_empty())
                        .map(|s| s.trim().to_owned())
                        .collect(),
                );
            }
            "--force" => force = true,
            _ if a.starts_with("--") => anyhow::bail!("unknown flag {a}"),
            _ => files.push(PathBuf::from(a)),
        }
    }

    Ok(Args {
        files,
        date,
        streams,
        data_dir,
        out,
        symbols,
        force,
    })
}

/// Open the source and report the input directories to guard against, mirroring
/// replayd's selector convention (explicit files, or `--date`+`--streams`).
fn open_source(args: &Args) -> anyhow::Result<(KqrSource, Vec<PathBuf>)> {
    if !args.files.is_empty() {
        let mut dirs: Vec<PathBuf> = args
            .files
            .iter()
            .filter_map(|f| f.parent().map(PathBuf::from))
            .collect();
        dirs.sort();
        dirs.dedup();
        return Ok((KqrSource::open_files(&args.files)?, dirs));
    }
    match &args.date {
        Some(date) if !args.streams.is_empty() => Ok((
            KqrSource::open_selector(&args.data_dir, date, &args.streams)?,
            vec![args.data_dir.clone()],
        )),
        _ => anyhow::bail!("give KQR files, or --date YYYYMMDD with --streams"),
    }
}

fn main() -> anyhow::Result<()> {
    let args = parse_args()?;

    let date = args
        .date
        .clone()
        .ok_or_else(|| anyhow::anyhow!("--date YYYYMMDD is required (it labels the output day)"))?;
    let out = args
        .out
        .clone()
        .ok_or_else(|| anyhow::anyhow!("--out DIR is required"))?;

    let (source, input_dirs) = open_source(&args)?;

    if let Some(dir) = out_inside_input(&out, &input_dirs) {
        anyhow::bail!(
            "--out {} is inside the input directory {}; refusing to write into the archive",
            out.display(),
            dir.display()
        );
    }
    if !args.force {
        let existing = existing_outputs(&out, &date);
        if !existing.is_empty() {
            let list: Vec<String> = existing.iter().map(|p| p.display().to_string()).collect();
            anyhow::bail!(
                "refusing to overwrite existing outputs (use --force): {}",
                list.join(", ")
            );
        }
    }

    let stats = convert(source, &date, &out, args.symbols.as_ref())?;

    println!(
        "kairos-kqr-export: records_in={} quotes={} trades={} unknown_variants={} \
         decode_errors={} frame_warnings={} scale_conflicts={} symbols={}",
        stats.records_in,
        stats.quotes,
        stats.trades,
        stats.unknown_variants,
        stats.decode_errors,
        stats.frame_warnings,
        stats.scale_conflicts,
        stats.hft_files.len(),
    );
    println!(
        "kairos-kqr-export: wrote {} ({} rows)",
        stats.quotes_path.display(),
        stats.quotes
    );
    println!(
        "kairos-kqr-export: wrote {} ({} rows)",
        stats.trades_path.display(),
        stats.trades
    );
    for f in &stats.hft_files {
        println!(
            "kairos-kqr-export: wrote {} ({} events)",
            f.path.display(),
            f.events
        );
    }
    Ok(())
}
