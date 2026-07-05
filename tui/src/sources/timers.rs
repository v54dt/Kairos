use anyhow::{Context, Result};
use tokio::process::Command;

/// One row of `systemctl --user list-timers` (columns NEXT LEFT LAST PASSED
/// UNIT ACTIVATES). NEXT/LAST are `Weekday YYYY-MM-DD HH:MM:SS TZ` timestamps or
/// `-`/`n/a`, LEFT/PASSED are multi-word durations, and systemd right-aligns the
/// duration columns, so rows are parsed by field grammar, not by header offsets.
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

fn is_ymd(t: &str) -> bool {
    let b = t.as_bytes();
    b.len() == 10
        && b[4] == b'-'
        && b[7] == b'-'
        && b[..4].iter().all(u8::is_ascii_digit)
        && b[5..7].iter().all(u8::is_ascii_digit)
        && b[8..].iter().all(u8::is_ascii_digit)
}

fn is_hms(t: &str) -> bool {
    let b = t.as_bytes();
    b.len() == 8
        && b[2] == b':'
        && b[5] == b':'
        && b[..2].iter().all(u8::is_ascii_digit)
        && b[3..5].iter().all(u8::is_ascii_digit)
        && b[6..].iter().all(u8::is_ascii_digit)
}

fn is_dash(t: &str) -> bool {
    t == "-" || t == "n/a"
}

/// A NEXT/LAST value is `Weekday YYYY-MM-DD HH:MM:SS TZ` (four whitespace tokens).
fn is_timestamp_at(tok: &[&str], i: usize) -> bool {
    i + 4 <= tok.len() && is_ymd(tok[i + 1]) && is_hms(tok[i + 2])
}

/// Consume a NEXT/LAST field: a four-token timestamp or a single `-`/`n/a` token.
fn take_time_or_dash(tok: &[&str], i: &mut usize) -> String {
    if is_timestamp_at(tok, *i) {
        let s = tok[*i..*i + 4].join(" ");
        *i += 4;
        s
    } else if *i < tok.len() {
        let s = tok[*i].to_string();
        *i += 1;
        s
    } else {
        String::new()
    }
}

/// Consume a LEFT duration: tokens up to where the LAST field begins (a
/// timestamp or a dash). A lone dash is the whole duration.
fn take_duration(tok: &[&str], i: &mut usize) -> String {
    let start = *i;
    while *i < tok.len() {
        if is_timestamp_at(tok, *i) {
            break;
        }
        let cur_is_dash = is_dash(tok[*i]);
        if cur_is_dash && *i > start {
            break;
        }
        *i += 1;
        if cur_is_dash {
            break;
        }
    }
    tok[start..*i].join(" ")
}

fn parse_row(line: &str) -> Option<TimerRow> {
    let tok: Vec<&str> = line.split_whitespace().collect();
    if tok.len() < 6 {
        return None;
    }
    let n = tok.len();
    let activates = tok[n - 1].to_string();
    let unit = tok[n - 2].to_string();
    let mid = &tok[..n - 2];
    let mut i = 0;
    let next = take_time_or_dash(mid, &mut i);
    let left = take_duration(mid, &mut i);
    let last = take_time_or_dash(mid, &mut i);
    let passed = mid[i..].join(" ");
    Some(TimerRow {
        next,
        left,
        last,
        passed,
        unit,
        activates,
    })
}

pub fn parse_list_timers(text: &str) -> Vec<TimerRow> {
    let mut lines = text.lines();
    loop {
        match lines.next() {
            Some(l) if l.contains("NEXT") && l.contains("UNIT") && l.contains("ACTIVATES") => break,
            Some(_) => continue,
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
        if let Some(row) = parse_row(line) {
            out.push(row);
        }
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

    // Verbatim `systemctl --user list-timers` output: systemd right-aligns the
    // LEFT/PASSED duration columns, so LEFT values wider than the header slot
    // ("1 day 3h") overflow leftward toward NEXT — the case offset slicing garbled.
    fn sample() -> String {
        let mut s = String::new();
        s.push_str("NEXT                             LEFT LAST PASSED UNIT                       ACTIVATES\n");
        s.push_str("Sun 2026-07-05 14:00:00 CST   3h left Sat 2026-07-04 14:00:00 CST  21h ago kairos-record-ship.timer   kairos-record-ship.service\n");
        s.push_str("n/a                               n/a Sat 2026-07-04 09:00:00 CST 1 day ago kairos-lab-blacklist.timer kairos-lab-blacklist.service\n");
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
    fn parses_right_aligned_multiday_left_column() {
        // Verbatim capture from this box; LEFT values wider than the header slot
        // ("1 day 3h", "1 day 10h") right-align and previously spilled into NEXT.
        let text = "\
NEXT                             LEFT LAST PASSED UNIT                       ACTIVATES
Mon 2026-07-06 09:00:00 CST       22h -         - kairos-lab-ticks.timer     kairos-lab-ticks.service
Mon 2026-07-06 14:35:00 CST  1 day 3h -         - kairos-record-ship.timer   kairos-record-ship.service
Mon 2026-07-06 21:00:00 CST 1 day 10h -         - kairos-lab-chips.timer     kairos-lab-chips.service

3 timers listed.
";
        let rows = parse_list_timers(text);
        assert_eq!(rows.len(), 3);
        assert_eq!(rows[0].next, "Mon 2026-07-06 09:00:00 CST");
        assert_eq!(rows[0].left, "22h");
        assert_eq!(rows[1].next, "Mon 2026-07-06 14:35:00 CST");
        assert_eq!(rows[1].left, "1 day 3h");
        assert_eq!(rows[1].last, "-");
        assert_eq!(rows[1].passed, "-");
        assert_eq!(rows[1].unit, "kairos-record-ship.timer");
        assert_eq!(rows[1].activates, "kairos-record-ship.service");
        assert_eq!(rows[2].next, "Mon 2026-07-06 21:00:00 CST");
        assert_eq!(rows[2].left, "1 day 10h");
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
