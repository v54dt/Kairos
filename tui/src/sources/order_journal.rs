use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use super::hub_status::HubReport;
use super::json::{int as json_int, string as json_str};

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

/// `YYYYMMDD` for the day `offset` days from now in UTC+8 (TWSE session date),
/// matching the engine's `DateFromUtc`. Hand-rolled to avoid a chrono dependency.
fn date_tw(offset: i64) -> String {
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0);
    let days = (secs + 8 * 3600).div_euclid(86_400) + offset;
    let (y, m, d) = civil_from_days(days);
    format!("{y:04}{m:02}{d:02}")
}

/// `YYYYMMDD` for the current TWSE session date (UTC+8).
pub fn today_tw() -> String {
    date_tw(0)
}

/// `YYYYMMDD` for the previous TWSE session date (UTC+8).
pub fn yesterday_tw() -> String {
    date_tw(-1)
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

/// One individual fill (never aggregated) for the "today's fills" panel.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Fill {
    pub t: i64,       // event time, micros
    pub stem: String, // journal file stem, e.g. "2330-Buy-20260705"
    pub buy: bool,    // side derived from the signed shares (>0 = BUY)
    pub shares: i64,  // absolute shares
    pub price: i64,   // Cents
}

impl Fill {
    /// The symbol segment of the journal file stem, for the detail's scenario
    /// column.
    pub fn symbol(&self) -> String {
        split_stem(&self.stem).0
    }
}

/// Every fill line in one journal file as its own row (torn/zero-share lines
/// skipped). Order preserved as written.
pub fn fills_in_file(stem: &str, text: &str) -> Vec<Fill> {
    let mut out = Vec::new();
    for line in text.lines() {
        if let Some(Event::Fill { shares, price, t }) = parse_journal_line(line)
            && shares != 0
        {
            out.push(Fill {
                t,
                stem: stem.to_string(),
                buy: shares > 0,
                shares: shares.abs(),
                price,
            });
        }
    }
    out
}

/// Scan `dir` for today's journal files and return every fill as a flat list,
/// sorted by time ascending (newest at the bottom). Missing dir => empty.
pub fn scan_fills(dir: &Path, date: &str) -> Vec<Fill> {
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
            out.extend(fills_in_file(&stem, &text));
        }
    }
    out.sort_by(|a, b| a.t.cmp(&b.t).then_with(|| a.stem.cmp(&b.stem)));
    out
}

/// TW fee/tax model, mirroring `exec/scenario/src/market/tw_fees.h`. Amounts in
/// TWD are whole units (無條件捨去 / floor), converted to cents for accounting.
#[derive(Clone, Debug)]
pub struct FeeParams {
    pub base_rate: f64,               // brokerage base rate (0.001425)
    pub discount: f64,                // broker discount multiplier (<=1.0)
    pub min_fee_roundlot_cents: i128, // round-lot minimum fee (20 TWD)
    pub min_fee_oddlot_cents: i128,   // odd-lot minimum fee (1 TWD)
    pub sell_tax_rate: f64,           // securities transaction tax (0.003)
    pub daytrade_tax_rate: f64,       // day-trade sell tax (0.0015); NOT applied, see settle()
    pub daytrade: bool,               // NOT derivable from the journal; kept false
}

impl Default for FeeParams {
    fn default() -> Self {
        FeeParams {
            base_rate: 0.001425,
            discount: 1.0,
            min_fee_roundlot_cents: 2000,
            min_fee_oddlot_cents: 100,
            sell_tax_rate: 0.003,
            daytrade_tax_rate: 0.0015,
            daytrade: false,
        }
    }
}

/// Brokerage fee (cents) for one fill: floor(notional_twd * base_rate *
/// discount) in whole TWD, floored to the per-fill minimum. Odd-lot heuristic:
/// a `shares` count that is not a positive multiple of 1000 is treated as an
/// odd-lot fill (1 TWD floor) since the journal line carries only shares, not the
/// board/scenario config; a round-lot fill keeps the 20 TWD floor.
fn fee_cents(notional_cents: i128, shares: i64, p: &FeeParams) -> i128 {
    let is_oddlot = shares > 0 && shares % 1000 != 0;
    let floor = if is_oddlot {
        p.min_fee_oddlot_cents
    } else {
        p.min_fee_roundlot_cents
    };
    let value = notional_cents as f64 / 100.0;
    let raw_twd = (value * p.base_rate * p.discount).floor() as i128;
    (raw_twd * 100).max(floor)
}

/// Securities transaction tax (cents) for one SELL fill: floor(notional_twd *
/// rate) in whole TWD. The journal carries no day-trade signal, so `p.daytrade`
/// stays false and the full 0.3% rate is applied (see the panel footnote) rather
/// than guessed; the day-trade plumbing exists only for a future explicit config.
fn sell_tax_cents(notional_cents: i128, p: &FeeParams) -> i128 {
    let rate = if p.daytrade {
        p.daytrade_tax_rate
    } else {
        p.sell_tax_rate
    };
    let value = notional_cents as f64 / 100.0;
    (value * rate).floor() as i128 * 100
}

/// Estimated TW settlement over today's fills. All fields are cents; the
/// payable/receivable/net getters derive the T+2 交割 figures.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Settlement {
    pub buy_notional_cents: i128,
    pub buy_fee_cents: i128,
    pub sell_notional_cents: i128,
    pub sell_fee_cents: i128,
    pub sell_tax_cents: i128,
}

impl Settlement {
    /// 買進 交割應付 (cash out), a negative number.
    pub fn payable_cents(&self) -> i128 {
        -(self.buy_notional_cents + self.buy_fee_cents)
    }

    /// 賣出 交割應收 (cash in), a positive number.
    pub fn receivable_cents(&self) -> i128 {
        self.sell_notional_cents - self.sell_fee_cents - self.sell_tax_cents
    }

    /// 淨交割金額 (net, T+2) = 應收 - 應付.
    pub fn net_cents(&self) -> i128 {
        self.receivable_cents() + self.payable_cents()
    }
}

/// Aggregate the settlement figures over a fill list. Fee/tax are charged per
/// fill (min fee applies to each round-lot fill).
pub fn settle(fills: &[Fill], p: &FeeParams) -> Settlement {
    let mut s = Settlement::default();
    for f in fills {
        let notional = f.shares as i128 * f.price as i128;
        if f.buy {
            s.buy_notional_cents += notional;
            s.buy_fee_cents += fee_cents(notional, f.shares, p);
        } else {
            s.sell_notional_cents += notional;
            s.sell_fee_cents += fee_cents(notional, f.shares, p);
            s.sell_tax_cents += sell_tax_cents(notional, p);
        }
    }
    s
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
    fn yesterday_is_before_today() {
        let y = yesterday_tw();
        assert_eq!(y.len(), 8);
        assert!(y < today_tw());
    }

    #[test]
    fn civil_epoch_and_known_date() {
        assert_eq!(civil_from_days(0), (1970, 1, 1));
        // 2026-07-05 is day 20639 since the Unix epoch.
        assert_eq!(civil_from_days(20639), (2026, 7, 5));
    }

    // Shared cross-language golden generated from the C++ canonical helper; the
    // yyyymmdd derived via this module's civil_from_days must agree with every row.
    // (tui has no hhmm helper, so the hhmm column is not asserted here.)
    #[test]
    fn golden_trading_days_yyyymmdd_match() {
        const F: &str = include_str!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../schema/testdata/trading_days.txt"
        ));
        let mut rows = 0;
        for line in F.lines() {
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let f: Vec<&str> = line.split('|').collect();
            assert_eq!(f.len(), 3, "bad row: {line}");
            let ts: i64 = f[0].parse().unwrap();
            let days = (ts.div_euclid(1_000_000) + 8 * 3600).div_euclid(86_400);
            let (y, m, d) = civil_from_days(days);
            assert_eq!(format!("{y:04}{m:02}{d:02}"), f[1], "row: {line}");
            rows += 1;
        }
        assert!(rows >= 10, "fixture shrank: {rows}");
    }

    // Shared cross-language golden: parse every line of journal_corpus.jsonl (the
    // exact C++ writer output) and assert the decoded fields match journal_expected.txt
    // plus the hostile-string round-trips. See schema/testdata/README.
    #[test]
    fn golden_journal_corpus_decodes() {
        const CORPUS: &str = include_str!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../schema/testdata/journal_corpus.jsonl"
        ));
        const EXPECTED: &str = include_str!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../schema/testdata/journal_expected.txt"
        ));
        let lines: Vec<&str> = CORPUS.lines().filter(|l| !l.is_empty()).collect();
        let rows: Vec<&str> = EXPECTED
            .lines()
            .filter(|l| !l.is_empty() && !l.starts_with('#'))
            .collect();
        assert_eq!(lines.len(), rows.len(), "corpus/expected length mismatch");

        let opt = |v: &str| (v != "-").then(|| v.parse::<i64>().unwrap());
        for (line, row) in lines.iter().zip(rows.iter()) {
            let f: Vec<&str> = row.split('|').collect();
            assert_eq!(f.len(), 4, "bad expected row: {row}");
            assert!(parse_journal_line(line).is_some(), "unparsable: {line}");
            assert_eq!(
                json_str(line, "type").as_deref(),
                Some(f[0]),
                "type: {line}"
            );
            if let Some(want) = opt(f[1]) {
                assert_eq!(json_int(line, "shares"), Some(want), "shares: {line}");
            }
            if let Some(want) = opt(f[2]) {
                assert_eq!(json_int(line, "price"), Some(want), "price: {line}");
            }
            if let Some(want) = opt(f[3]) {
                assert_eq!(json_int(line, "ok"), Some(want), "ok: {line}");
            }
        }

        // Hostile ack `err` (line 4) round-trips exactly through the C++ escaper and
        // this parser: newline, tab, quote, backslash, a C0 control (U+001C), CJK.
        let hostile = "reject:\nline2\ttab \"q\" \\slash \u{1c} sep 台積";
        assert_eq!(json_str(lines[3], "err").as_deref(), Some(hostile));

        // Truncation cap (line 5): the escaped err ends with the truncation marker.
        assert!(
            json_str(lines[4], "err")
                .unwrap()
                .ends_with("...(truncated)")
        );

        // FINDING: OrderJournal::LogAck does NOT JsonEscape `id`, so a quote in the
        // id produces malformed JSON; the reader silently truncates at the raw quote.
        // (OrderFlowJournal::AppendAck escapes correctly -- see line 4 above.)
        assert_eq!(json_str(lines[10], "id").as_deref(), Some("bad"));

        // aggregate_file over the whole corpus: only "fill"/"cancel" types count.
        let agg = aggregate_file("2330-Buy-20260101", CORPUS);
        assert_eq!(agg.fills, 2);
        assert_eq!(agg.filled_shares, 3000);
        assert_eq!(agg.filled_notional_cents, 174_200_000_i128);
        assert_eq!(agg.cancels, 1);
        assert_eq!(agg.other, 8);
    }

    #[test]
    fn fills_one_row_per_fill_with_side_from_sign() {
        let text = "\
{\"t\":10,\"type\":\"ack\",\"id\":\"k1-1\",\"ok\":1}
{\"t\":20,\"type\":\"fill\",\"id\":\"k1-1\",\"shares\":3000,\"price\":58500}
{\"t\":30,\"type\":\"fill\",\"id\":\"k1-2\",\"shares\":-1000,\"price\":41250}
{\"t\":40,\"type\":\"fill\",\"id\":\"k1-3\",\"shares\":0,\"price\":100}
{\"t\":50,\"type\":\"fill\",\"id\":\"k1-4\",\"shares\":200"; // torn: skipped
        let fills = fills_in_file("2330-Buy-20260705", text);
        assert_eq!(fills.len(), 2, "zero-share and torn lines are dropped");
        assert!(fills[0].buy);
        assert_eq!(fills[0].shares, 3000);
        assert_eq!(fills[0].price, 58500);
        assert_eq!(fills[0].symbol(), "2330");
        assert!(!fills[1].buy, "negative shares => SELL");
        assert_eq!(fills[1].shares, 1000);
    }

    fn buy(shares: i64, price: i64) -> Fill {
        Fill {
            t: 0,
            stem: "2330-Buy-20260705".to_string(),
            buy: true,
            shares,
            price,
        }
    }

    fn sell(shares: i64, price: i64) -> Fill {
        Fill {
            t: 0,
            stem: "2327-Sell-20260705".to_string(),
            buy: false,
            shares,
            price,
        }
    }

    #[test]
    fn settlement_buy_and_sell_exact() {
        // BUY 3,000 @585.00: notional 1,755,000 TWD; fee floor(1,755,000*0.001425)
        //   = floor(2500.875) = 2,500 TWD.
        // SELL 1,000 @412.50: notional 412,500 TWD; fee floor(587.8125)=587;
        //   tax floor(412,500*0.003)=floor(1237.5)=1,237 TWD.
        let s = settle(
            &[buy(3000, 58500), sell(1000, 41250)],
            &FeeParams::default(),
        );
        assert_eq!(s.buy_notional_cents, 175_500_000);
        assert_eq!(s.buy_fee_cents, 250_000);
        assert_eq!(s.sell_notional_cents, 41_250_000);
        assert_eq!(s.sell_fee_cents, 58_700);
        assert_eq!(s.sell_tax_cents, 123_700);
        // 交割應付 = -(1,755,000 + 2,500) = -1,757,500 TWD.
        assert_eq!(s.payable_cents(), -175_750_000);
        // 交割應收 = 412,500 - 587 - 1,237 = 410,676 TWD.
        assert_eq!(s.receivable_cents(), 41_067_600);
        // 淨 = 410,676 - 1,757,500 = -1,346,824 TWD.
        assert_eq!(s.net_cents(), -134_682_400);
    }

    #[test]
    fn settlement_min_fee_floor_per_roundlot() {
        // BUY 1,000 @10.00 (a round lot): notional 10,000 TWD; raw fee
        // floor(14.25)=14 TWD, floored up to the 20 TWD round-lot minimum.
        let s = settle(&[buy(1000, 1000)], &FeeParams::default());
        assert_eq!(s.buy_fee_cents, 2000);
    }

    #[test]
    fn settlement_oddlot_fill_uses_one_twd_floor() {
        // BUY 37 shares @10.00 (not a 1000-multiple => odd-lot heuristic):
        // notional 370 TWD; raw fee floor(370*0.001425)=floor(0.527)=0 TWD,
        // floored up to the 1 TWD odd-lot minimum (100 cents), NOT the 20 TWD
        // round-lot floor.
        let s = settle(&[buy(37, 1000)], &FeeParams::default());
        assert_eq!(s.buy_fee_cents, 100);

        // A larger odd lot whose raw fee already clears the 1 TWD floor keeps the
        // raw fee: SELL 1,500 @412.50 = 618,750 TWD; raw floor(881.72)=881 TWD.
        let s = settle(&[sell(1500, 41250)], &FeeParams::default());
        assert_eq!(s.sell_fee_cents, 88_100);
    }

    #[test]
    fn settlement_buy_only_has_no_receivable() {
        let s = settle(&[buy(3000, 58500)], &FeeParams::default());
        assert_eq!(s.sell_notional_cents, 0);
        assert_eq!(s.receivable_cents(), 0);
        assert_eq!(s.payable_cents(), -175_750_000);
        assert_eq!(s.net_cents(), -175_750_000);
    }

    #[test]
    fn settlement_sell_only_has_no_payable() {
        let s = settle(&[sell(1000, 41250)], &FeeParams::default());
        assert_eq!(s.buy_notional_cents, 0);
        assert_eq!(s.payable_cents(), 0);
        assert_eq!(s.receivable_cents(), 41_067_600);
        assert_eq!(s.net_cents(), 41_067_600);
    }

    #[test]
    fn scan_fills_sorted_ascending_by_time() {
        let dir = std::env::temp_dir().join(format!("kairos-fills-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let date = today_tw();
        std::fs::write(
            dir.join(format!("2330-Buy-{date}.jsonl")),
            "{\"t\":200,\"type\":\"fill\",\"id\":\"a\",\"shares\":3000,\"price\":58500}\n",
        )
        .unwrap();
        std::fs::write(
            dir.join(format!("2327-Sell-{date}.jsonl")),
            "{\"t\":100,\"type\":\"fill\",\"id\":\"b\",\"shares\":-1000,\"price\":41250}\n",
        )
        .unwrap();
        let fills = scan_fills(&dir, &date);
        std::fs::remove_dir_all(&dir).ok();
        assert_eq!(fills.len(), 2);
        assert_eq!(fills[0].t, 100, "earliest fill first");
        assert!(!fills[0].buy);
        assert_eq!(fills[1].t, 200);
        assert!(fills[1].buy);
    }
}
