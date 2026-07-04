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

/// The most recent parseable stats line in a journal dump (last stream printed
/// in the latest reporting cycle).
pub fn parse_latest_stats(text: &str) -> Option<RecorderStats> {
    text.lines().rev().find_map(parse_stats_line)
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

pub async fn tail_stats(unit: &str, lines: u32) -> Result<Option<RecorderStats>> {
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
    fn latest_stats_takes_last_match() {
        let text = "\
kairos-recordd: stream 1001 records=1 bytes=10 drops=0 write_errs=0 disk_free_mib=100
kairos-recordd: stream 1002 records=2 bytes=20 drops=1 write_errs=0 disk_free_mib=100
kairos-recordd: shutting down
";
        let s = parse_latest_stats(text).unwrap();
        assert_eq!(s.stream, 1002);
        assert_eq!(s.drops, 1);
    }

    #[test]
    fn disk_free_of_root_is_some() {
        assert!(disk_free_bytes(Path::new("/")).is_some());
        assert!(disk_free_bytes(Path::new("/nonexistent-kairos-xyz")).is_none());
    }
}
