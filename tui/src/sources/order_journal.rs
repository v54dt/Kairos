use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use super::hub_status::HubReport;

/// Aggregated view of one B2 OrderJournal file (`<symbol>-<Side>-<YYYYMMDD>.jsonl`).
/// One file per (symbol, side, day) — not strictly one per scenario.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ScenarioJournal {
    pub name: String, // file stem, e.g. "2330-Buy-20260705"
    pub symbol: String,
    pub side: String,
    pub fills: u64,
    pub filled_shares: i64,
    pub filled_notional_cents: i128, // sum(shares * price); price is Cents
    pub cancels: u64,                // successful cancels (re-peg proxy)
    pub other: u64,                  // acks + unknown event kinds
    pub last_event_us: i64,
}

/// The Scenarios tab view: journal-derived rows plus the live hub section. Either
/// half may be absent; the panel renders placeholders, never crashes.
#[derive(Clone, Debug, Default)]
pub struct ScenariosView {
    pub scenarios: Vec<ScenarioJournal>,
    pub hub: Option<HubReport>,
}

enum Event {
    Fill { shares: i64, price: i64, t: i64 },
    Cancel { ok: bool, t: i64 },
    Other { t: i64 },
}

fn json_int(s: &str, key: &str) -> Option<i64> {
    let needle = format!("\"{key}\":");
    let start = s.find(&needle)? + needle.len();
    let rest = &s[start..];
    let end = rest
        .find(|c: char| c != '-' && !c.is_ascii_digit())
        .unwrap_or(rest.len());
    rest[..end].parse().ok()
}

fn json_str(s: &str, key: &str) -> Option<String> {
    let needle = format!("\"{key}\":\"");
    let start = s.find(&needle)? + needle.len();
    let rest = &s[start..];
    let end = rest.find('"')?;
    Some(rest[..end].to_string())
}

/// Parse one JSONL event line. A torn/mid-append line (no closing `}`) yields
/// `None` and is skipped; an unrecognised `type` is `Event::Other`.
fn parse_journal_line(line: &str) -> Option<Event> {
    let line = line.trim_end();
    if !line.ends_with('}') || !line.starts_with('{') {
        return None; // torn last line
    }
    let t = json_int(line, "t").unwrap_or(0);
    match json_str(line, "type").as_deref() {
        Some("fill") => Some(Event::Fill {
            shares: json_int(line, "shares").unwrap_or(0),
            price: json_int(line, "price").unwrap_or(0),
            t,
        }),
        Some("cancel") => Some(Event::Cancel {
            ok: json_int(line, "ok").unwrap_or(0) != 0,
            t,
        }),
        _ => Some(Event::Other { t }),
    }
}

fn split_stem(stem: &str) -> (String, String) {
    let parts: Vec<&str> = stem.split('-').collect();
    // <symbol>-<Side>-<YYYYMMDD>
    if parts.len() >= 3 {
        (parts[0].to_string(), parts[1].to_string())
    } else {
        (stem.to_string(), String::new())
    }
}

/// Aggregate all lines of one journal file into a scenario row.
pub fn aggregate_file(stem: &str, text: &str) -> ScenarioJournal {
    let (symbol, side) = split_stem(stem);
    let mut j = ScenarioJournal {
        name: stem.to_string(),
        symbol,
        side,
        ..Default::default()
    };
    for line in text.lines() {
        match parse_journal_line(line) {
            Some(Event::Fill { shares, price, t }) => {
                if shares != 0 {
                    j.fills += 1;
                    j.filled_shares += shares;
                    j.filled_notional_cents += shares as i128 * price as i128;
                }
                j.last_event_us = j.last_event_us.max(t);
            }
            Some(Event::Cancel { ok, t }) => {
                if ok {
                    j.cancels += 1;
                }
                j.last_event_us = j.last_event_us.max(t);
            }
            Some(Event::Other { t }) => {
                j.other += 1;
                j.last_event_us = j.last_event_us.max(t);
            }
            None => {} // torn line: skip
        }
    }
    j
}

/// `YYYYMMDD` for the current day in UTC+8 (TWSE session date), matching the
/// engine's `DateFromUtc`. Hand-rolled to avoid a chrono dependency.
pub fn today_tw() -> String {
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0);
    let days = (secs + 8 * 3600).div_euclid(86_400);
    let (y, m, d) = civil_from_days(days);
    format!("{y:04}{m:02}{d:02}")
}

// Days since 1970-01-01 -> (year, month, day). Howard Hinnant's civil algorithm.
fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (if m <= 2 { y + 1 } else { y }, m, d)
}

/// Scan `dir` for today's journal files and aggregate each. Missing dir => empty.
pub fn scan_journal(dir: &Path, date: &str) -> Vec<ScenarioJournal> {
    let suffix = format!("-{date}.jsonl");
    let mut out = Vec::new();
    let entries = match std::fs::read_dir(dir) {
        Ok(e) => e,
        Err(_) => return out,
    };
    for entry in entries.flatten() {
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if !name.ends_with(&suffix) {
            continue;
        }
        let stem = name.trim_end_matches(".jsonl").to_string();
        if let Ok(text) = std::fs::read_to_string(entry.path()) {
            out.push(aggregate_file(&stem, &text));
        }
    }
    out.sort_by(|a, b| a.name.cmp(&b.name));
    out
}

/// Combine the two independently-sourced halves into one view. Either may be
/// absent (empty scenarios / `None` hub) without producing an error.
pub fn merge(scenarios: Vec<ScenarioJournal>, hub: Option<HubReport>) -> ScenariosView {
    ScenariosView { scenarios, hub }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sources::hub_status::HubStatus;
    use std::time::Duration;

    #[test]
    fn parses_fill_line() {
        let line = "{\"t\":1720000000000000,\"type\":\"fill\",\"id\":\"k1-1\",\"shares\":300,\
                    \"price\":58000}";
        match parse_journal_line(line) {
            Some(Event::Fill { shares, price, t }) => {
                assert_eq!(shares, 300);
                assert_eq!(price, 58000);
                assert_eq!(t, 1_720_000_000_000_000);
            }
            _ => panic!("expected fill"),
        }
    }

    #[test]
    fn torn_last_line_skipped() {
        // mid-append: no closing brace
        assert!(parse_journal_line("{\"t\":1,\"type\":\"fill\",\"shares\":30").is_none());
        assert!(parse_journal_line("").is_none());
        assert!(parse_journal_line("garbage").is_none());
    }

    #[test]
    fn unknown_type_is_other() {
        let line = "{\"t\":5,\"type\":\"heartbeat\"}";
        assert!(matches!(
            parse_journal_line(line),
            Some(Event::Other { t: 5 })
        ));
    }

    #[test]
    fn aggregate_totals_per_scenario() {
        let text = "\
{\"t\":10,\"type\":\"ack\",\"id\":\"k1-1\",\"ok\":1}
{\"t\":20,\"type\":\"fill\",\"id\":\"k1-1\",\"shares\":1000,\"price\":5800}
{\"t\":30,\"type\":\"fill\",\"id\":\"k1-1\",\"shares\":500,\"price\":5800}
{\"t\":40,\"type\":\"cancel\",\"id\":\"k1-2\",\"ok\":1}
{\"t\":50,\"type\":\"quirk\"}
{\"t\":60,\"type\":\"fill\",\"id\":\"k1-3\",\"shares\":200"; // torn trailing line
        let j = aggregate_file("2330-Buy-20260705", text);
        assert_eq!(j.symbol, "2330");
        assert_eq!(j.side, "Buy");
        assert_eq!(j.fills, 2);
        assert_eq!(j.filled_shares, 1500);
        assert_eq!(j.filled_notional_cents, 1500 * 5800);
        assert_eq!(j.cancels, 1);
        assert_eq!(j.other, 2); // ack + quirk
        assert_eq!(j.last_event_us, 50); // torn fill line ignored
    }

    #[test]
    fn failed_cancel_not_counted() {
        let text = "{\"t\":1,\"type\":\"cancel\",\"id\":\"k1-1\",\"ok\":0}";
        let j = aggregate_file("x-Buy-20260705", text);
        assert_eq!(j.cancels, 0);
    }

    fn hub() -> HubReport {
        HubReport {
            status: HubStatus {
                client_count: 1,
                ..Default::default()
            },
            age: Duration::from_secs(1),
        }
    }

    #[test]
    fn merge_both_present() {
        let v = merge(vec![ScenarioJournal::default()], Some(hub()));
        assert_eq!(v.scenarios.len(), 1);
        assert!(v.hub.is_some());
    }

    #[test]
    fn merge_journal_only() {
        let v = merge(vec![ScenarioJournal::default()], None);
        assert_eq!(v.scenarios.len(), 1);
        assert!(v.hub.is_none());
    }

    #[test]
    fn merge_hub_only() {
        let v = merge(vec![], Some(hub()));
        assert!(v.scenarios.is_empty());
        assert!(v.hub.is_some());
    }

    #[test]
    fn merge_neither() {
        let v = merge(vec![], None);
        assert!(v.scenarios.is_empty());
        assert!(v.hub.is_none());
    }

    #[test]
    fn today_is_eight_digits() {
        let d = today_tw();
        assert_eq!(d.len(), 8);
        assert!(d.chars().all(|c| c.is_ascii_digit()));
    }

    #[test]
    fn civil_epoch_and_known_date() {
        assert_eq!(civil_from_days(0), (1970, 1, 1));
        // 2026-07-05 is day 20639 since the Unix epoch.
        assert_eq!(civil_from_days(20639), (2026, 7, 5));
    }
}
