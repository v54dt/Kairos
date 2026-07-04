use anyhow::{Context, Result};
use tokio::process::Command;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LogLine {
    pub ts: String,
    pub unit: String,
    pub message: String,
}

/// Parse `journalctl -o short-iso` output for one unit. Each line is
/// `<iso-ts> <host> <ident>[<pid>]: <message>`; meta lines starting with `--`
/// (e.g. `-- No entries --`) are dropped.
pub fn parse_short_iso(unit: &str, text: &str) -> Vec<LogLine> {
    text.lines()
        .map(str::trim)
        .filter(|l| !l.is_empty() && !l.starts_with("--"))
        .filter_map(|l| {
            let ts = l.split_whitespace().next()?.to_string();
            let message = l.split_once(": ").map(|(_, m)| m).unwrap_or(l).to_string();
            Some(LogLine {
                ts,
                unit: unit.to_string(),
                message,
            })
        })
        .collect()
}

pub async fn tail_unit(unit: &str, lines: u32) -> Result<Vec<LogLine>> {
    let output = Command::new("journalctl")
        .args([
            "--user",
            "-u",
            unit,
            "-n",
            &lines.to_string(),
            "--no-pager",
            "-o",
            "short-iso",
        ])
        .output()
        .await
        .context("spawn journalctl")?;
    if !output.status.success() {
        anyhow::bail!("journalctl exited with {}", output.status);
    }
    Ok(parse_short_iso(
        unit,
        &String::from_utf8_lossy(&output.stdout),
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = "\
2026-07-04T11:57:54+08:00 ubuntu kairos-recordd[3127138]: kairos-recordd: stream 1002 records=12 bytes=672 drops=0
2026-07-04T11:57:56+08:00 ubuntu systemd[2685]: Stopping kairos-recordd.service - Kairos KQR recorder...
2026-07-04T11:57:56+08:00 ubuntu kairos-recordd[3127138]: kairos-recordd: shutting down
";

    #[test]
    fn parses_ts_and_message() {
        let logs = parse_short_iso("kairos-recordd.service", SAMPLE);
        assert_eq!(logs.len(), 3);
        assert_eq!(logs[0].ts, "2026-07-04T11:57:54+08:00");
        assert_eq!(logs[0].unit, "kairos-recordd.service");
        assert_eq!(
            logs[0].message,
            "kairos-recordd: stream 1002 records=12 bytes=672 drops=0"
        );
        assert_eq!(logs[2].message, "kairos-recordd: shutting down");
    }

    #[test]
    fn drops_meta_and_blank_lines() {
        let text = "-- No entries --\n\n";
        assert!(parse_short_iso("kairos-core.service", text).is_empty());
    }

    #[test]
    fn line_without_colon_keeps_whole_message() {
        let logs = parse_short_iso("u", "2026-07-04T11:57:54+08:00 barelog");
        assert_eq!(logs.len(), 1);
        assert_eq!(logs[0].message, "2026-07-04T11:57:54+08:00 barelog");
    }
}
