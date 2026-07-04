use std::collections::BTreeMap;
use std::path::Path;

use anyhow::{Context, Result};
use tokio::process::Command;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RecorderStats {
    pub stream: i32,
    pub records: u64,
    pub bytes: u64,
    pub drops: u64,
    pub write_errs: u64,
    pub disk_free_mib: u64,
}

fn field(line: &str, key: &str) -> Option<u64> {
    line.split_whitespace()
        .find_map(|t| t.strip_prefix(key)?.parse().ok())
}

/// Parse one `kairos-recordd: stream <id> records=.. bytes=.. drops=..
/// write_errs=.. disk_free_mib=..` stats line (any journal prefix ignored).
pub fn parse_stats_line(line: &str) -> Option<RecorderStats> {
    let mut toks = line.split_whitespace();
    let mut stream = None;
    while let Some(t) = toks.next() {
        if t == "stream" {
            stream = toks.next().and_then(|s| s.parse().ok());
            break;
        }
    }
    Some(RecorderStats {
        stream: stream?,
        records: field(line, "records=")?,
        bytes: field(line, "bytes=")?,
        drops: field(line, "drops=")?,
        write_errs: field(line, "write_errs=")?,
        disk_free_mib: field(line, "disk_free_mib=")?,
    })
}

/// The most recent stats line for each stream in a journal dump, sorted by
/// stream id, so drops on any stream stay visible (not just the last printed).
pub fn parse_latest_stats(text: &str) -> Vec<RecorderStats> {
    let mut latest: BTreeMap<i32, RecorderStats> = BTreeMap::new();
    for line in text.lines() {
        if let Some(s) = parse_stats_line(line) {
            latest.insert(s.stream, s);
        }
    }
    latest.into_values().collect()
}

/// Free bytes on the filesystem holding `path`, or None if it can't be queried.
pub fn disk_free_bytes(path: &Path) -> Option<u64> {
    use std::os::unix::ffi::OsStrExt;
    let c = std::ffi::CString::new(path.as_os_str().as_bytes()).ok()?;
    // SAFETY: `st` is zeroed and `c` is a valid NUL-terminated path.
    let mut st: libc::statvfs = unsafe { std::mem::zeroed() };
    if unsafe { libc::statvfs(c.as_ptr(), &mut st) } != 0 {
        return None;
    }
    Some(st.f_bavail.saturating_mul(st.f_frsize))
}

pub async fn tail_stats(unit: &str, lines: u32) -> Result<Vec<RecorderStats>> {
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
    Ok(parse_latest_stats(&String::from_utf8_lossy(&output.stdout)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_prefixed_stats_line() {
        let line = "2026-07-04T11:57:54+08:00 ubuntu kairos-recordd[31]: kairos-recordd: \
                    stream 1002 records=12 bytes=672 drops=0 write_errs=0 disk_free_mib=576149";
        let s = parse_stats_line(line).unwrap();
        assert_eq!(
            s,
            RecorderStats {
                stream: 1002,
                records: 12,
                bytes: 672,
                drops: 0,
                write_errs: 0,
                disk_free_mib: 576_149,
            }
        );
    }

    #[test]
    fn non_stats_line_is_none() {
        assert!(parse_stats_line("kairos-recordd: shutting down").is_none());
        assert!(parse_stats_line("").is_none());
    }

    #[test]
    fn latest_stats_keeps_every_stream() {
        let text = "\
kairos-recordd: stream 1001 records=1 bytes=10 drops=0 write_errs=0 disk_free_mib=100
kairos-recordd: stream 1002 records=2 bytes=20 drops=0 write_errs=0 disk_free_mib=100
kairos-recordd: stream 1001 records=9 bytes=90 drops=42 write_errs=3 disk_free_mib=100
kairos-recordd: stream 1002 records=8 bytes=80 drops=0 write_errs=0 disk_free_mib=100
kairos-recordd: shutting down
";
        let stats = parse_latest_stats(text);
        assert_eq!(stats.len(), 2);
        // Sorted by stream id; data stream 1001 keeps its latest drops.
        assert_eq!(stats[0].stream, 1001);
        assert_eq!(stats[0].records, 9);
        assert_eq!(stats[0].drops, 42);
        assert_eq!(stats[0].write_errs, 3);
        assert_eq!(stats[1].stream, 1002);
        assert_eq!(stats[1].records, 8);
        assert_eq!(stats[1].drops, 0);
    }

    #[test]
    fn latest_stats_empty_when_no_match() {
        assert!(parse_latest_stats("kairos-recordd: shutting down").is_empty());
    }

    #[test]
    fn disk_free_of_root_is_some() {
        assert!(disk_free_bytes(Path::new("/")).is_some());
        assert!(disk_free_bytes(Path::new("/nonexistent-kairos-xyz")).is_none());
    }
}
