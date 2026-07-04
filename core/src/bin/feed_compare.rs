//! Offline dual-sidecar consistency check (D2 bring-up harness).
//!
//! Usage:
//!   kairos-feed-compare <a.kqr> <b.kqr>   compare two single-source recordings
//!   kairos-feed-compare <both.kqr>        compare the two `source`s inside one file
//!
//! Prints seq gaps, cross-source trade/book mismatches, and trade-timestamp skew,
//! then exits non-zero if any divergence (gaps or mismatches) is found.

use std::process::ExitCode;

use kairos_core::compare::{compare_streams, read_kqr_events};
use kairos_core::decode::FeedEvent;

fn source_of(ev: &FeedEvent) -> u16 {
    match ev {
        FeedEvent::Quote(q) => q.source,
        FeedEvent::Trade(t) => t.source,
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let (a, b) = match args.as_slice() {
        [one] => {
            let events = match read_kqr_events(&read(one)) {
                Ok(e) => e,
                Err(e) => return fail(&format!("{one}: {e}")),
            };
            let sources: Vec<u16> = {
                let mut s: Vec<u16> = events.iter().map(source_of).collect();
                s.sort_unstable();
                s.dedup();
                s
            };
            if sources.len() != 2 {
                return fail(&format!(
                    "single-file mode needs exactly two sources, found {}",
                    sources.len()
                ));
            }
            let (lo, hi) = (sources[0], sources[1]);
            let a: Vec<FeedEvent> = events
                .iter()
                .filter(|e| source_of(e) == lo)
                .cloned()
                .collect();
            let b: Vec<FeedEvent> = events
                .iter()
                .filter(|e| source_of(e) == hi)
                .cloned()
                .collect();
            (a, b)
        }
        [pa, pb] => {
            let a = match read_kqr_events(&read(pa)) {
                Ok(e) => e,
                Err(e) => return fail(&format!("{pa}: {e}")),
            };
            let b = match read_kqr_events(&read(pb)) {
                Ok(e) => e,
                Err(e) => return fail(&format!("{pb}: {e}")),
            };
            (a, b)
        }
        _ => {
            eprintln!("usage: kairos-feed-compare <a.kqr> <b.kqr> | <both.kqr>");
            return ExitCode::from(2);
        }
    };

    let report = compare_streams(&a, &b);

    for g in &report.seq_gaps {
        println!(
            "seq-gap source={} symbol={} epoch={} {} -> {}",
            g.source, g.symbol, g.epoch, g.prev_seq, g.next_seq
        );
    }
    for m in &report.trade_mismatches {
        println!(
            "trade-mismatch symbol={} #{} a=({}e-{},vol {}) b=({}e-{},vol {})",
            m.symbol,
            m.index,
            m.a_price_mantissa,
            m.a_price_scale,
            m.a_volume,
            m.b_price_mantissa,
            m.b_price_scale,
            m.b_volume
        );
    }
    for m in &report.book_mismatches {
        println!(
            "book-mismatch symbol={} side={:?} a=({}e-{}) b=({}e-{})",
            m.symbol,
            m.side,
            m.a_price_mantissa,
            m.a_price_scale,
            m.b_price_mantissa,
            m.b_price_scale
        );
    }
    if let (Some(min), Some(max)) = (
        report.ts_deltas.iter().map(|d| d.delta_us).min(),
        report.ts_deltas.iter().map(|d| d.delta_us).max(),
    ) {
        println!(
            "ts-skew trades={} min={}us max={}us",
            report.ts_deltas.len(),
            min,
            max
        );
    }

    if report.is_clean() {
        println!("feed-compare: streams consistent");
        ExitCode::SUCCESS
    } else {
        println!(
            "feed-compare: DIVERGENCE ({} seq-gaps, {} trade-mismatches, {} book-mismatches)",
            report.seq_gaps.len(),
            report.trade_mismatches.len(),
            report.book_mismatches.len()
        );
        ExitCode::FAILURE
    }
}

fn read(path: &str) -> Vec<u8> {
    std::fs::read(path).unwrap_or_else(|e| {
        eprintln!("kairos-feed-compare: cannot read {path}: {e}");
        std::process::exit(2);
    })
}

fn fail(msg: &str) -> ExitCode {
    eprintln!("kairos-feed-compare: {msg}");
    ExitCode::from(2)
}
