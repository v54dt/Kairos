use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

// Read-only view of the F1 fail-closed blacklist gate (exec/scenario): if the
// file is missing, malformed, or stale the trader refuses to trade, so the
// panel surfaces exactly those conditions.

const DEFAULT_PATH: &str = "/home/coder/kairos-lab/data/blacklist/current.csv";
const MAX_STALE_DAYS: u64 = 4;

/// One active restriction: normalized symbol plus its category.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BlacklistEntry {
    pub symbol: String,
    pub category: String,
}

/// Freshness of the blacklist CSV as the trader would see it at startup.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BlacklistFreshness {
    pub path: String,
    pub present: bool,
    pub age: Option<Duration>,
    pub entry_count: Option<usize>,
    pub entries: Vec<BlacklistEntry>,
    pub stale: bool,
    pub malformed: bool,
    pub clock_anomaly: bool,
}

/// RFC4180 reader faithful to exec/scenario `ParseCsv`: quoted fields, embedded
/// commas, doubled-quote escaping, CRLF/LF, UTF-8 passthrough, leading BOM
/// stripped. Returns an error on an unterminated quoted field.
fn parse_csv(text: &str) -> Result<Vec<Vec<String>>, String> {
    let text = text.strip_prefix('\u{feff}').unwrap_or(text);
    let chars: Vec<char> = text.chars().collect();
    let n = chars.len();
    let mut rows: Vec<Vec<String>> = Vec::new();
    let mut row: Vec<String> = Vec::new();
    let mut field = String::new();
    let mut in_quotes = false;
    let mut field_started = false;
    let mut row_started = false;
    let mut i = 0;
    while i < n {
        let c = chars[i];
        if in_quotes {
            if c == '"' {
                if i + 1 < n && chars[i + 1] == '"' {
                    field.push('"');
                    i += 1;
                } else {
                    in_quotes = false;
                }
            } else {
                field.push(c);
            }
            i += 1;
            continue;
        }
        if c == '"' && !field_started {
            in_quotes = true;
            field_started = true;
            row_started = true;
        } else if c == ',' {
            row_started = true;
            row.push(std::mem::take(&mut field));
            field_started = false;
        } else if c == '\r' {
            // skip
        } else if c == '\n' {
            if row_started || !field.is_empty() {
                row.push(std::mem::take(&mut field));
                field_started = false;
                rows.push(std::mem::take(&mut row));
                row_started = false;
            }
        } else {
            field.push(c);
            field_started = true;
            row_started = true;
        }
        i += 1;
    }
    if in_quotes {
        return Err("unterminated quoted field".to_string());
    }
    if row_started || !field.is_empty() {
        row.push(field);
        rows.push(row);
    }
    Ok(rows)
}

const REQUIRED_COLS: [&str; 5] = ["symbol", "category", "note", "start_date", "end_date"];
const CATEGORIES: [&str; 5] = [
    "disposal",
    "attention",
    "suspension",
    "margin_suspension",
    "sell_first",
];

/// Trim ASCII whitespace and upper-case, matching exec/scenario `NormalizeSymbol`.
fn normalize_symbol(s: &str) -> String {
    s.trim_matches(|c: char| c.is_ascii_whitespace())
        .chars()
        .map(|c| c.to_ascii_uppercase())
        .collect()
}

/// Number of data rows (header excluded). Mirrors the trader's fail-closed
/// `Blacklist::Parse`: errors on a malformed CSV, a missing header column, a row
/// whose field count differs from the header, an embedded newline in a field, an
/// empty or non-ASCII-alphanumeric symbol, or an unknown category.
pub fn parse_entries(text: &str) -> Result<Vec<BlacklistEntry>, String> {
    let rows = parse_csv(text)?;
    let header = rows.first().ok_or("no header row")?;
    for name in REQUIRED_COLS {
        if !header.iter().any(|h| h == name) {
            return Err(format!("missing header column: {name}"));
        }
    }
    let i_symbol = header.iter().position(|h| h == "symbol").unwrap();
    let i_category = header.iter().position(|h| h == "category").unwrap();
    let mut entries = Vec::with_capacity(rows.len().saturating_sub(1));
    for (r, fields) in rows.iter().enumerate().skip(1) {
        if fields.len() != header.len() {
            return Err(format!(
                "row {r}: got {} fields, header has {}",
                fields.len(),
                header.len()
            ));
        }
        if fields.iter().any(|f| f.contains('\n') || f.contains('\r')) {
            return Err(format!("row {r}: embedded newline in field"));
        }
        let symbol = normalize_symbol(&fields[i_symbol]);
        if symbol.is_empty() {
            return Err(format!("row {r}: empty symbol"));
        }
        if !symbol
            .bytes()
            .all(|b| b.is_ascii_digit() || b.is_ascii_uppercase())
        {
            return Err(format!("row {r}: non-alphanumeric byte in symbol"));
        }
        if !CATEGORIES.contains(&fields[i_category].as_str()) {
            return Err(format!(
                "row {r}: unknown category '{}'",
                fields[i_category]
            ));
        }
        entries.push(BlacklistEntry {
            symbol,
            category: fields[i_category].clone(),
        });
    }
    Ok(entries)
}

/// Precedence: flag (if non-empty) > env `KAIROS_BLACKLIST_CSV` > lab default.
/// Mirrors exec/scenario `ResolveBlacklistPath`.
pub fn resolve_blacklist_path(flag: Option<&str>, env: Option<&str>) -> PathBuf {
    if let Some(p) = flag
        && !p.is_empty()
    {
        return PathBuf::from(p);
    }
    if let Some(e) = env
        && !e.is_empty()
    {
        return PathBuf::from(e);
    }
    PathBuf::from(DEFAULT_PATH)
}

/// Stat + parse the blacklist relative to `now`. Never panics: an absent path is
/// `present: false`, an unreadable/malformed file is `malformed: true`, a file
/// older than the 4-day gate threshold is `stale: true`, and a file whose mtime
/// is in the future is `clock_anomaly: true` (the trader fails closed on it).
pub fn read_blacklist(path: &Path, now: SystemTime) -> BlacklistFreshness {
    let mut f = BlacklistFreshness {
        path: path.display().to_string(),
        ..Default::default()
    };
    let meta = match std::fs::metadata(path) {
        Ok(m) if m.is_file() => m,
        _ => return f,
    };
    f.present = true;
    if let Ok(mtime) = meta.modified() {
        match now.duration_since(mtime) {
            Ok(age) => {
                f.stale = age > Duration::from_secs(MAX_STALE_DAYS * 86_400);
                f.age = Some(age);
            }
            // A future mtime is a clock anomaly; the trader fails closed on it.
            Err(_) => f.clock_anomaly = true,
        }
    }
    match std::fs::read_to_string(path) {
        Ok(text) => match parse_entries(&text) {
            Ok(entries) => {
                f.entry_count = Some(entries.len());
                f.entries = entries;
            }
            Err(_) => f.malformed = true,
        },
        Err(_) => f.malformed = true,
    }
    f
}

#[cfg(test)]
mod tests {
    use super::*;

    const HEADER: &str = "symbol,category,note,start_date,end_date\n";

    fn parse_entry_count(text: &str) -> Result<usize, String> {
        parse_entries(text).map(|e| e.len())
    }

    fn tmp_path(tag: &str) -> PathBuf {
        std::env::temp_dir().join(format!("kairos-bl-{}-{}.csv", std::process::id(), tag))
    }

    #[test]
    fn counts_data_rows() {
        let text = format!("{HEADER}2330,disposal,note,20260701,20260710\n2317,attention,x,,\n");
        assert_eq!(parse_entry_count(&text).unwrap(), 2);
    }

    #[test]
    fn entries_carry_symbol_and_category() {
        let text = format!("{HEADER}tsm,disposal,note,20260701,20260710\n2317,attention,x,,\n");
        let entries = parse_entries(&text).unwrap();
        assert_eq!(entries.len(), 2);
        assert_eq!(entries[0].symbol, "TSM");
        assert_eq!(entries[0].category, "disposal");
        assert_eq!(entries[1].symbol, "2317");
        assert_eq!(entries[1].category, "attention");
    }

    #[test]
    fn header_only_is_zero_entries() {
        assert_eq!(parse_entry_count(HEADER).unwrap(), 0);
    }

    #[test]
    fn bom_and_quoted_comma_field_is_one_entry() {
        let text = "\u{feff}symbol,category,note,start_date,end_date\n\
                    2330,disposal,\"note, with comma\",20260701,20260710\n";
        assert_eq!(parse_entry_count(text).unwrap(), 1);
    }

    #[test]
    fn unterminated_quote_is_malformed() {
        let text = format!("{HEADER}2330,disposal,\"oops,20260701,20260710\n");
        assert!(parse_entry_count(&text).is_err());
    }

    #[test]
    fn empty_text_has_no_header() {
        assert!(parse_entry_count("").is_err());
    }

    #[test]
    fn missing_header_column_is_error() {
        assert!(parse_entry_count("symbol,category\n2330,disposal\n").is_err());
    }

    #[test]
    fn short_row_is_error() {
        assert!(parse_entry_count(&format!("{HEADER}2330,disposal\n")).is_err());
    }

    #[test]
    fn unknown_category_is_error() {
        assert!(parse_entry_count(&format!("{HEADER}2330,dispsal,x,,\n")).is_err());
    }

    #[test]
    fn empty_symbol_is_error() {
        assert!(parse_entry_count(&format!("{HEADER},disposal,x,,\n")).is_err());
    }

    #[test]
    fn non_ascii_symbol_is_error() {
        assert!(parse_entry_count(&format!("{HEADER}２３３０,disposal,x,,\n")).is_err());
    }

    #[test]
    fn embedded_newline_in_quoted_field_is_error() {
        let text = format!("{HEADER}2330,disposal,\"line1\nline2\",,\n");
        assert!(parse_entry_count(&text).is_err());
    }

    #[test]
    fn lowercase_symbol_is_normalized_and_counted() {
        assert!(parse_entry_count(&format!("{HEADER}tsm,disposal,x,,\n")).is_ok());
    }

    #[test]
    fn resolve_precedence() {
        assert_eq!(
            resolve_blacklist_path(Some("/a.csv"), Some("/b.csv")),
            PathBuf::from("/a.csv")
        );
        assert_eq!(
            resolve_blacklist_path(Some(""), Some("/b.csv")),
            PathBuf::from("/b.csv")
        );
        assert_eq!(
            resolve_blacklist_path(None, None),
            PathBuf::from(DEFAULT_PATH)
        );
    }

    #[test]
    fn absent_file_is_not_present() {
        let f = read_blacklist(Path::new("/no/such/blacklist.csv"), SystemTime::now());
        assert!(!f.present);
        assert!(!f.stale);
        assert!(!f.malformed);
    }

    #[test]
    fn present_fresh_file_counts_entries() {
        let path = tmp_path("fresh");
        std::fs::write(&path, format!("{HEADER}2330,disposal,x,,\n")).unwrap();
        let f = read_blacklist(&path, SystemTime::now());
        let _ = std::fs::remove_file(&path);
        assert!(f.present);
        assert!(!f.stale);
        assert!(!f.malformed);
        assert_eq!(f.entry_count, Some(1));
        assert_eq!(f.entries.len(), 1);
        assert_eq!(f.entries[0].symbol, "2330");
    }

    #[test]
    fn old_file_is_stale() {
        let path = tmp_path("stale");
        std::fs::write(&path, HEADER).unwrap();
        let future = SystemTime::now() + Duration::from_secs(5 * 86_400);
        let f = read_blacklist(&path, future);
        let _ = std::fs::remove_file(&path);
        assert!(f.present);
        assert!(f.stale);
    }

    #[test]
    fn future_mtime_is_clock_anomaly_not_fresh() {
        let path = tmp_path("future");
        std::fs::write(&path, format!("{HEADER}2330,disposal,x,,\n")).unwrap();
        let past = SystemTime::now() - Duration::from_secs(5 * 86_400);
        let f = read_blacklist(&path, past);
        let _ = std::fs::remove_file(&path);
        assert!(f.present);
        assert!(f.clock_anomaly);
        assert!(!f.stale);
        assert!(f.age.is_none());
    }

    #[test]
    fn malformed_file_flagged() {
        let path = tmp_path("bad");
        std::fs::write(&path, format!("{HEADER}2330,disposal,\"oops,,\n")).unwrap();
        let f = read_blacklist(&path, SystemTime::now());
        let _ = std::fs::remove_file(&path);
        assert!(f.present);
        assert!(f.malformed);
    }
}
