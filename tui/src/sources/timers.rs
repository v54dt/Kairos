use anyhow::{Context, Result};
use tokio::process::Command;

/// One row of `systemctl --user list-timers` (columns NEXT LEFT LAST PASSED
/// UNIT ACTIVATES). Value columns hold multi-word timestamps, so rows are sliced
/// by the header's column offsets rather than by whitespace.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TimerRow {
    pub next: String,
    pub left: String,
    pub last: String,
    pub passed: String,
    pub unit: String,
    pub activates: String,
}

/// Last-run outcome of a service, from `systemctl show -p ExecMainStatus
/// -p ExecMainExitTimestamp`. An empty `exit_ts` means the service never ran.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ServiceResult {
    pub exec_main_status: i32,
    pub exit_ts: String,
}

/// A timer row joined with its activated service's last-run result (absent when
/// the `systemctl show` call failed).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TimerEntry {
    pub row: TimerRow,
    pub result: Option<ServiceResult>,
}

fn header_col(header: &str, name: &str) -> Option<usize> {
    header.find(name).map(|b| header[..b].chars().count())
}

fn col_span(chars: &[char], start: usize, end: usize) -> String {
    let end = end.min(chars.len());
    if start >= end {
        return String::new();
    }
    chars[start..end]
        .iter()
        .collect::<String>()
        .trim()
        .to_string()
}

pub fn parse_list_timers(text: &str) -> Vec<TimerRow> {
    let mut lines = text.lines();
    let header = loop {
        match lines.next() {
            Some(l) if l.contains("NEXT") && l.contains("UNIT") && l.contains("ACTIVATES") => {
                break l;
            }
            Some(_) => continue,
            None => return Vec::new(),
        }
    };
    let cols = ["NEXT", "LEFT", "LAST", "PASSED", "UNIT", "ACTIVATES"];
    let mut starts = Vec::with_capacity(cols.len());
    for name in cols {
        match header_col(header, name) {
            Some(s) => starts.push(s),
            None => return Vec::new(),
        }
    }
    let mut out = Vec::new();
    for line in lines {
        if line.trim().is_empty() {
            break;
        }
        // Skip the trailing "N timers listed." footer and any legend text.
        if !line.contains(".timer") {
            continue;
        }
        let chars: Vec<char> = line.chars().collect();
        out.push(TimerRow {
            next: col_span(&chars, starts[0], starts[1]),
            left: col_span(&chars, starts[1], starts[2]),
            last: col_span(&chars, starts[2], starts[3]),
            passed: col_span(&chars, starts[3], starts[4]),
            unit: col_span(&chars, starts[4], starts[5]),
            activates: col_span(&chars, starts[5], usize::MAX),
        });
    }
    out
}

pub fn parse_show_kv(text: &str) -> ServiceResult {
    let mut r = ServiceResult::default();
    for line in text.lines() {
        if let Some((k, v)) = line.split_once('=') {
            match k.trim() {
                "ExecMainStatus" => r.exec_main_status = v.trim().parse().unwrap_or(0),
                "ExecMainExitTimestamp" => r.exit_ts = v.trim().to_string(),
                _ => {}
            }
        }
    }
    r
}

pub async fn list_timers() -> Result<Vec<TimerRow>> {
    let output = Command::new("systemctl")
        .args(["--user", "list-timers", "kairos-*", "--all", "--no-pager"])
        .output()
        .await
        .context("spawn systemctl")?;
    if !output.status.success() {
        anyhow::bail!("systemctl exited with {}", output.status);
    }
    Ok(parse_list_timers(&String::from_utf8_lossy(&output.stdout)))
}

pub async fn service_result(svc: &str) -> Result<ServiceResult> {
    let output = Command::new("systemctl")
        .args([
            "--user",
            "show",
            svc,
            "-p",
            "ExecMainStatus,ExecMainExitTimestamp",
        ])
        .output()
        .await
        .context("spawn systemctl")?;
    if !output.status.success() {
        anyhow::bail!("systemctl exited with {}", output.status);
    }
    Ok(parse_show_kv(&String::from_utf8_lossy(&output.stdout)))
}

/// Every kairos-* timer joined with its activated service's last-run result.
pub async fn collect_timers() -> Result<Vec<TimerEntry>> {
    let rows = list_timers().await?;
    let mut out = Vec::with_capacity(rows.len());
    for row in rows {
        let result = service_result(&row.activates).await.ok();
        out.push(TimerEntry { row, result });
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    // Widths matching systemd's left-aligned columns; building the fixture with
    // format! guarantees the header offsets line up with the value columns.
    const W: [usize; 5] = [28, 10, 28, 12, 27];

    fn line(f: [&str; 6]) -> String {
        let mut s = String::new();
        for (i, field) in f.iter().enumerate() {
            match W.get(i) {
                Some(w) => s.push_str(&format!("{field:<w$}")),
                None => s.push_str(field),
            }
        }
        s.push('\n');
        s
    }

    fn sample() -> String {
        let mut s = String::new();
        s.push_str(&line([
            "NEXT",
            "LEFT",
            "LAST",
            "PASSED",
            "UNIT",
            "ACTIVATES",
        ]));
        s.push_str(&line([
            "Sun 2026-07-05 14:00:00 CST",
            "3h left",
            "Sat 2026-07-04 14:00:00 CST",
            "21h ago",
            "kairos-record-ship.timer",
            "kairos-record-ship.service",
        ]));
        s.push_str(&line([
            "n/a",
            "n/a",
            "Sat 2026-07-04 09:00:00 CST",
            "1 day ago",
            "kairos-lab-blacklist.timer",
            "kairos-lab-blacklist.service",
        ]));
        s.push('\n');
        s.push_str("2 timers listed.\n");
        s
    }

    #[test]
    fn parses_columns() {
        let sample = sample();
        let rows = parse_list_timers(&sample);
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0].next, "Sun 2026-07-05 14:00:00 CST");
        assert_eq!(rows[0].left, "3h left");
        assert_eq!(rows[0].last, "Sat 2026-07-04 14:00:00 CST");
        assert_eq!(rows[0].passed, "21h ago");
        assert_eq!(rows[0].unit, "kairos-record-ship.timer");
        assert_eq!(rows[0].activates, "kairos-record-ship.service");
    }

    #[test]
    fn parses_waiting_timer_with_na() {
        let rows = parse_list_timers(&sample());
        assert_eq!(rows[1].next, "n/a");
        assert_eq!(rows[1].left, "n/a");
        assert_eq!(rows[1].passed, "1 day ago");
        assert_eq!(rows[1].unit, "kairos-lab-blacklist.timer");
        assert_eq!(rows[1].activates, "kairos-lab-blacklist.service");
    }

    #[test]
    fn stops_at_blank_and_skips_footer() {
        // Footer line has no ".timer" and the blank line ends the table.
        let rows = parse_list_timers(&sample());
        assert!(rows.iter().all(|r| r.unit.ends_with(".timer")));
    }

    #[test]
    fn empty_or_headerless_input_yields_no_rows() {
        assert!(parse_list_timers("").is_empty());
        assert!(parse_list_timers("0 timers listed.\n").is_empty());
    }

    #[test]
    fn parses_service_result() {
        let text = "ExecMainStatus=0\nExecMainExitTimestamp=Sat 2026-07-04 15:30:12 CST\n";
        let r = parse_show_kv(text);
        assert_eq!(r.exec_main_status, 0);
        assert_eq!(r.exit_ts, "Sat 2026-07-04 15:30:12 CST");
    }

    #[test]
    fn parses_nonzero_status() {
        let r = parse_show_kv("ExecMainStatus=1\nExecMainExitTimestamp=\n");
        assert_eq!(r.exec_main_status, 1);
        assert!(r.exit_ts.is_empty());
    }

    #[test]
    fn missing_keys_default() {
        let r = parse_show_kv("Id=kairos-record-ship.service\n");
        assert_eq!(r.exec_main_status, 0);
        assert!(r.exit_ts.is_empty());
    }
}
