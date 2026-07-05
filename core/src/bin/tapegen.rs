//! kairos-tapegen — generate a synthetic KQR market-data tape (stream 1001) for
//! off-hours TUI / trading-engine tests. The tape is byte-deterministic from its
//! inputs and its timestamps stay in the continuous TWSE session 09:00-13:25 Taipei
//! (unless `--allow-out-of-window`), so the offline fill model treats it as live.
//!
//! Usage:
//!   kairos-tapegen --scenario <name> --symbol S --out FILE
//!     [--date YYYYMMDD] [--seconds N] [--tick-ms N] [--seed N]
//!     [--base-price MANTISSA] [--scale N] [--allow-out-of-window]
//! Scenarios: quotes-only (no trades) | trend-day (fills a join BUY) |
//!            limit-lock (pinned at the limit, unbuyable).

use mimalloc::MiMalloc;

#[global_allocator]
static GLOBAL: MiMalloc = MiMalloc;

use std::fs::File;
use std::io::BufWriter;
use std::path::PathBuf;

use kairos_core::record::{FileHeader, RecordWriter};
use kairos_core::tapegen::{self, GenParams, STREAM_ID, Scenario, hhmm_from_us};

struct Args {
    scenario: Scenario,
    symbol: String,
    out: PathBuf,
    date: u32,
    seconds: u32,
    tick_ms: u32,
    seed: u64,
    base_mantissa: i64,
    scale: u8,
    allow_out_of_window: bool,
}

fn parse_args() -> anyhow::Result<Args> {
    let mut scenario: Option<Scenario> = None;
    let mut symbol = String::from("2330");
    let mut out: Option<PathBuf> = None;
    let mut date = 20_260_703u32;
    let mut seconds = 120u32;
    let mut tick_ms = 1000u32;
    let mut seed = 1u64;
    let mut base_mantissa = 58_000i64;
    let mut scale = 2u8;
    let mut allow_out_of_window = false;

    let mut it = std::env::args().skip(1);
    while let Some(a) = it.next() {
        let mut next = || {
            it.next()
                .ok_or_else(|| anyhow::anyhow!("{a} needs a value"))
        };
        match a.as_str() {
            "--scenario" => {
                scenario = Some(Scenario::parse(&next()?).map_err(|e| anyhow::anyhow!(e))?)
            }
            "--symbol" => symbol = next()?,
            "--out" => out = Some(PathBuf::from(next()?)),
            "--date" => date = next()?.parse::<u32>()?,
            "--seconds" => seconds = next()?.parse::<u32>()?,
            "--tick-ms" => tick_ms = next()?.parse::<u32>()?,
            "--seed" => seed = next()?.parse::<u64>()?,
            "--base-price" => base_mantissa = next()?.parse::<i64>()?,
            "--scale" => scale = next()?.parse::<u8>()?,
            "--allow-out-of-window" => allow_out_of_window = true,
            "--speed-hint" => {
                let _ = next()?; // advisory only; replay pace is chosen at replay time
            }
            _ if a.starts_with("--") => anyhow::bail!("unknown flag {a}"),
            _ => anyhow::bail!("unexpected positional argument {a}"),
        }
    }

    if tick_ms == 0 {
        anyhow::bail!("--tick-ms must be positive");
    }
    Ok(Args {
        scenario: scenario.ok_or_else(|| anyhow::anyhow!("--scenario is required"))?,
        symbol,
        out: out.ok_or_else(|| anyhow::anyhow!("--out is required"))?,
        date,
        seconds,
        tick_ms,
        seed,
        base_mantissa,
        scale,
        allow_out_of_window,
    })
}

fn hms(ts_us: i64) -> String {
    let hhmm = hhmm_from_us(ts_us);
    let secs = ((ts_us / 1_000_000) % 60 + 60) % 60;
    format!("{:02}:{:02}:{:02}", hhmm / 100, hhmm % 100, secs)
}

fn main() -> anyhow::Result<()> {
    let args = parse_args()?;
    let params = GenParams {
        scenario: args.scenario,
        symbol: args.symbol.clone(),
        date: args.date,
        seconds: args.seconds,
        tick_ms: args.tick_ms,
        seed: args.seed,
        base_mantissa: args.base_mantissa,
        scale: args.scale,
        allow_out_of_window: args.allow_out_of_window,
    };

    let file = File::create(&args.out)?;
    let mut writer = RecordWriter::create(
        BufWriter::new(file),
        &FileHeader::new(STREAM_ID, params.session_open_us()),
    )?;
    let stats = match tapegen::write_tape(&mut writer, &params) {
        Ok(s) => s,
        Err(e) => {
            drop(writer);
            let _ = std::fs::remove_file(&args.out);
            anyhow::bail!("{e}");
        }
    };
    writer.flush()?;

    println!(
        "kairos-tapegen: scenario={} symbol={} out={} records={} quotes={} trades={} ts_window={}..{} date={}",
        args.scenario.name(),
        args.symbol,
        args.out.display(),
        stats.records,
        stats.quotes,
        stats.trades,
        hms(stats.first_ts_us),
        hms(stats.last_ts_us),
        args.date,
    );
    Ok(())
}
