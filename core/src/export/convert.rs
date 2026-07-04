//! The A5 conversion driver: streams a `KqrSource`, decodes each fragment with the
//! shared `decode_feed_event`, and fans Quotes/Trades out to the Parquet writers
//! and the hftbacktest accumulator. Unknown/future variants and decode errors are
//! counted and skipped, never aborting the run (A2 metrics philosophy).

use std::collections::HashSet;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};

use anyhow::Result;

use super::hft::{HftAccumulator, write_symbol_npy};
use super::parquet::{QuoteRow, QuotesWriter, TradeRow, TradesWriter};
use crate::decode::{DecodeError, FeedEvent, decode_feed_event};
use crate::replay::KqrSource;

/// One hftbacktest array written for a (symbol, day).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HftFileStat {
    pub symbol: String,
    pub path: PathBuf,
    pub events: u64,
}

/// Tally of a conversion run.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConvertStats {
    pub records_in: u64,
    pub quotes: u64,
    pub trades: u64,
    pub unknown_variants: u64,
    pub decode_errors: u64,
    pub scale_conflicts: u64,
    pub frame_warnings: u64,
    pub quotes_path: PathBuf,
    pub trades_path: PathBuf,
    pub hft_files: Vec<HftFileStat>,
}

/// The two fixed parquet paths and the hft directory for a run.
pub fn planned_targets(out_dir: &Path, day: &str) -> (PathBuf, PathBuf, PathBuf) {
    (
        out_dir.join("quotes").join(format!("{day}.parquet")),
        out_dir.join("trades").join(format!("{day}.parquet")),
        out_dir.join("hft"),
    )
}

/// Existing output files a run would overwrite: the two parquet tables plus any
/// `*_<day>_v1.npy` already in the hft dir. Used by the bin to refuse (write
/// nothing) unless `--force`.
pub fn existing_outputs(out_dir: &Path, day: &str) -> Vec<PathBuf> {
    let (quotes, trades, hft) = planned_targets(out_dir, day);
    let mut hits = Vec::new();
    if quotes.exists() {
        hits.push(quotes);
    }
    if trades.exists() {
        hits.push(trades);
    }
    let suffix = format!("_{day}_v1.npy");
    if let Ok(rd) = std::fs::read_dir(&hft) {
        for entry in rd.flatten() {
            let p = entry.path();
            if p.file_name()
                .and_then(|n| n.to_str())
                .is_some_and(|n| n.ends_with(&suffix))
            {
                hits.push(p);
            }
        }
    }
    hits.sort();
    hits
}

/// Canonicalize a path even if it does not yet exist, by canonicalizing its
/// nearest existing ancestor and re-appending the remaining components.
fn absolutize(p: &Path) -> PathBuf {
    if let Ok(c) = p.canonicalize() {
        return c;
    }
    let mut tail: Vec<std::ffi::OsString> = Vec::new();
    let mut cur = p.to_path_buf();
    while let Some(parent) = cur.parent().map(Path::to_path_buf) {
        if let Some(name) = cur.file_name() {
            tail.push(name.to_owned());
        }
        if let Ok(mut base) = parent.canonicalize() {
            for name in tail.iter().rev() {
                base.push(name);
            }
            return base;
        }
        if parent.as_os_str().is_empty() {
            break;
        }
        cur = parent;
    }
    std::env::current_dir()
        .map(|d| d.join(p))
        .unwrap_or_else(|_| p.to_path_buf())
}

/// Returns the offending input directory if `out` is equal to or nested inside any
/// input directory (so the converter can never write into the raw KQR archive).
pub fn out_inside_input(out: &Path, input_dirs: &[PathBuf]) -> Option<PathBuf> {
    let out_abs = absolutize(out);
    input_dirs.iter().find_map(|d| {
        let in_abs = absolutize(d);
        (out_abs == in_abs || out_abs.starts_with(&in_abs)).then_some(in_abs)
    })
}

fn included(filter: Option<&HashSet<String>>, symbol: &str) -> bool {
    filter.is_none_or(|s| s.contains(symbol))
}

/// Delete `*_<day>_v1.npy` files this run did not regenerate, so a narrowed
/// re-run cannot leave orphan per-symbol arrays that desync the hft dir from the
/// freshly rewritten Parquet tables.
fn remove_stale_hft(hft_dir: &Path, day: &str, written: &[HftFileStat]) -> Result<()> {
    let keep: HashSet<&Path> = written.iter().map(|s| s.path.as_path()).collect();
    let suffix = format!("_{day}_v1.npy");
    if let Ok(rd) = std::fs::read_dir(hft_dir) {
        for entry in rd.flatten() {
            let p = entry.path();
            let is_day_npy = p
                .file_name()
                .and_then(|n| n.to_str())
                .is_some_and(|n| n.ends_with(&suffix));
            if is_day_npy && !keep.contains(p.as_path()) {
                std::fs::remove_file(&p)?;
            }
        }
    }
    Ok(())
}

/// Convert every record from `source` into the Parquet tables and hft arrays under
/// `out_dir`, labelled with `day`. `symbols`, when given, keeps only those symbols.
pub fn convert(
    mut source: KqrSource,
    day: &str,
    out_dir: &Path,
    symbols: Option<&HashSet<String>>,
) -> Result<ConvertStats> {
    let (quotes_path, trades_path, hft_dir) = planned_targets(out_dir, day);
    std::fs::create_dir_all(quotes_path.parent().unwrap())?;
    std::fs::create_dir_all(trades_path.parent().unwrap())?;
    std::fs::create_dir_all(&hft_dir)?;

    let mut qw = QuotesWriter::create(&quotes_path)?;
    let mut tw = TradesWriter::create(&trades_path)?;
    let mut acc = HftAccumulator::new();

    let mut records_in = 0u64;
    let mut quotes = 0u64;
    let mut trades = 0u64;
    let mut unknown_variants = 0u64;
    let mut decode_errors = 0u64;
    let mut scale_conflicts = 0u64;

    for rec in source.by_ref() {
        records_in += 1;
        match decode_feed_event(&rec.payload) {
            Ok(FeedEvent::Quote(q)) => {
                if !included(symbols, &q.symbol) {
                    continue;
                }
                quotes += 1;
                let (row, conflict) = QuoteRow::from_quote(&q, rec.recv_ts_us);
                if conflict {
                    scale_conflicts += 1;
                }
                qw.push(row)?;
                acc.add_quote(&q, rec.recv_ts_us);
            }
            Ok(FeedEvent::Trade(t)) => {
                if !included(symbols, &t.symbol) {
                    continue;
                }
                trades += 1;
                tw.push(TradeRow::from_trade(&t, rec.recv_ts_us))?;
                acc.add_trade(&t, rec.recv_ts_us);
            }
            Err(DecodeError::UnknownVariant) => unknown_variants += 1,
            Err(_) => decode_errors += 1,
        }
    }

    let frame_warnings = source.warnings();
    qw.finish()?;
    tw.finish()?;

    let mut hft_files = Vec::new();
    for (symbol, events) in acc.into_sorted() {
        let path = hft_dir.join(format!("{symbol}_{day}_v1.npy"));
        let mut f = BufWriter::new(File::create(&path)?);
        write_symbol_npy(&mut f, &events)?;
        f.flush()?;
        hft_files.push(HftFileStat {
            symbol,
            path,
            events: events.len() as u64,
        });
    }
    remove_stale_hft(&hft_dir, day, &hft_files)?;

    Ok(ConvertStats {
        records_in,
        quotes,
        trades,
        unknown_variants,
        decode_errors,
        scale_conflicts,
        frame_warnings,
        quotes_path,
        trades_path,
        hft_files,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::encode::encode_quote;
    use crate::model::{Exchange, PriceLevel, Quote, QuoteBoard, Session};
    use crate::record::{FileHeader, RecordWriter};
    use crate::replay::KqrSource;
    use std::fs::File;

    fn quote(sym: &str) -> Quote {
        Quote {
            symbol: sym.to_owned(),
            exchange: Exchange::Twse,
            quote_ts_us: 100,
            bids: vec![PriceLevel {
                price_mantissa: 100,
                price_scale: 2,
                volume: 1,
            }],
            asks: vec![],
            last_price: 0,
            last_scale: 0,
            last_volume: 0,
            is_trial: false,
            source: 0,
            seq: 1,
            epoch: 1,
            recv_ts_us: 0,
            board: QuoteBoard::RoundLot,
            session: Session::Day,
            trading_date: 0,
            simtrade: false,
            underlying_price: 0,
        }
    }

    #[test]
    fn narrowed_rerun_removes_stale_npy() {
        let base = std::env::temp_dir().join(format!("kairos-stale-{}", std::process::id()));
        let input = base.join("in");
        let out = base.join("out");
        std::fs::create_dir_all(&input).unwrap();
        let kqr = input.join("s1001-20260704.kqr");
        let mut w =
            RecordWriter::create(File::create(&kqr).unwrap(), &FileHeader::new(1001, 0)).unwrap();
        w.append(10, &encode_quote(&quote("2330"))).unwrap();
        w.append(20, &encode_quote(&quote("2317"))).unwrap();
        w.flush().unwrap();
        drop(w);

        let src = KqrSource::open_files(std::slice::from_ref(&kqr)).unwrap();
        convert(src, "20260704", &out, None).unwrap();
        let n2317 = out.join("hft").join("2317_20260704_v1.npy");
        assert!(n2317.exists());

        let filter: HashSet<String> = ["2330".to_owned()].into_iter().collect();
        let src = KqrSource::open_files(std::slice::from_ref(&kqr)).unwrap();
        convert(src, "20260704", &out, Some(&filter)).unwrap();
        assert!(
            !n2317.exists(),
            "stale 2317 npy must be removed on a narrowed rerun"
        );

        std::fs::remove_dir_all(&base).ok();
    }

    #[test]
    fn out_inside_input_is_detected() {
        let base = std::env::temp_dir().join(format!("kairos-guard-{}", std::process::id()));
        let data = base.join("data");
        std::fs::create_dir_all(&data).unwrap();
        let inside = data.join("kqr").join("out");
        assert!(out_inside_input(&inside, std::slice::from_ref(&data)).is_some());
        let outside = base.join("export");
        assert!(out_inside_input(&outside, std::slice::from_ref(&data)).is_none());
        std::fs::remove_dir_all(&base).ok();
    }
}
