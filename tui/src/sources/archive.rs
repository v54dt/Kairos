use std::path::Path;
use std::time::SystemTime;

use anyhow::{Context, Result};
use tokio::process::Command;

/// A recorder .kqr file on disk under `<data-dir>/kqr/<yyyymmdd>/`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct KqrFile {
    pub name: String,
    pub size: u64,
    pub mtime: SystemTime,
}

/// One scan of the recorder archive: today's live .kqr files plus whether
/// yesterday's day dir holds a compressed (.kqr.zst) or bare (.kqr) file.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ArchiveScan {
    pub today_files: Vec<KqrFile>,
    pub yesterday_compressed: bool,
    pub yesterday_uncompressed: bool,
}

/// Last ship/verify outcome parsed from the ship service's journal.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ShipVerify {
    pub ok: bool,
    pub detail: String,
    pub ts: String,
}

pub fn scan_archive(kqr_dir: &Path, today: &str, yesterday: &str) -> ArchiveScan {
    let mut scan = ArchiveScan::default();
    if let Ok(entries) = std::fs::read_dir(kqr_dir.join(today)) {
        for e in entries.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            if !name.ends_with(".kqr") {
                continue;
            }
            let (size, mtime) = e
                .metadata()
                .map(|m| (m.len(), m.modified().unwrap_or(SystemTime::UNIX_EPOCH)))
                .unwrap_or((0, SystemTime::UNIX_EPOCH));
            scan.today_files.push(KqrFile { name, size, mtime });
        }
    }
    scan.today_files.sort_by(|a, b| a.name.cmp(&b.name));
    if let Ok(entries) = std::fs::read_dir(kqr_dir.join(yesterday)) {
        for e in entries.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            if name.ends_with(".kqr.zst") {
                scan.yesterday_compressed = true;
            } else if name.ends_with(".kqr") {
                scan.yesterday_uncompressed = true;
            }
        }
    }
    scan
}

fn classify_ship_verify(ts: &str, msg: &str) -> Option<ShipVerify> {
    let mk = |ok: bool, detail: String| {
        Some(ShipVerify {
            ok,
            detail,
            ts: ts.to_string(),
        })
    };
    if let Some(rest) = msg.strip_prefix("kairos-record-verify: ") {
        if let Some((path, _)) = rest.split_once(" OK ") {
            return mk(true, path.trim().to_string());
        }
        if let Some((path, err)) = rest.split_once(" FAILED: ") {
            return mk(false, format!("{} ({})", path.trim(), err.trim()));
        }
        return None;
    }
    if let Some(rest) = msg.strip_prefix("kairos-record-ship: ") {
        if let Some(zst) = rest.strip_prefix("VERIFY FAILED: ") {
            return mk(false, zst.trim().to_string());
        }
        if rest.starts_with("done (") {
            return mk(true, rest.trim().to_string());
        }
    }
    None
}

/// Latest ship/verify line in a `journalctl -o short-iso` dump of the ship unit.
/// Recognises both the record-verify subprocess lines and the ship wrapper's own
/// lines; the last match wins. `None` when no such line is present.
pub fn parse_ship_verify(text: &str) -> Option<ShipVerify> {
    let mut latest = None;
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with("--") {
            continue;
        }
        let ts = line.split_whitespace().next().unwrap_or("");
        let msg = line.split_once(": ").map(|(_, m)| m).unwrap_or(line);
        if let Some(sv) = classify_ship_verify(ts, msg) {
            latest = Some(sv);
        }
    }
    latest
}

pub async fn ship_verify(unit: &str, lines: u32) -> Result<Option<ShipVerify>> {
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
    Ok(parse_ship_verify(&String::from_utf8_lossy(&output.stdout)))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp_dir(tag: &str) -> std::path::PathBuf {
        let p = std::env::temp_dir().join(format!("kairos-arc-{}-{}", std::process::id(), tag));
        let _ = std::fs::remove_dir_all(&p);
        p
    }

    #[test]
    fn scans_today_and_yesterday() {
        let kqr = tmp_dir("scan");
        let today = "20260705";
        let yesterday = "20260704";
        std::fs::create_dir_all(kqr.join(today)).unwrap();
        std::fs::create_dir_all(kqr.join(yesterday)).unwrap();
        std::fs::write(kqr.join(today).join("s1001-20260705.kqr"), b"aaaa").unwrap();
        std::fs::write(kqr.join(today).join("s1002-20260705.kqr"), b"bb").unwrap();
        std::fs::write(kqr.join(today).join("notes.txt"), b"skip").unwrap();
        std::fs::write(
            kqr.join(yesterday).join("s1001-20260704.kqr.zst"),
            b"zzz",
        )
        .unwrap();

        let scan = scan_archive(&kqr, today, yesterday);
        let _ = std::fs::remove_dir_all(&kqr);

        assert_eq!(scan.today_files.len(), 2);
        assert_eq!(scan.today_files[0].name, "s1001-20260705.kqr");
        assert_eq!(scan.today_files[0].size, 4);
        assert!(scan.yesterday_compressed);
        assert!(!scan.yesterday_uncompressed);
    }

    #[test]
    fn missing_dirs_yield_empty_scan() {
        let scan = scan_archive(Path::new("/no/such/kqr"), "20260705", "20260704");
        assert!(scan.today_files.is_empty());
        assert!(!scan.yesterday_compressed);
        assert!(!scan.yesterday_uncompressed);
    }

    const PREFIX: &str = "2026-07-04T14:00:01+08:00 host kairos-record-ship[42]: ";

    #[test]
    fn parses_verify_ok() {
        let text = format!(
            "{PREFIX}kairos-record-verify: /data/kqr/20260704/s1001-20260704.kqr.zst OK \
             stream=1001 records=5 bytes=50 recv_ts=[1..2]\n"
        );
        let sv = parse_ship_verify(&text).unwrap();
        assert!(sv.ok);
        assert_eq!(sv.detail, "/data/kqr/20260704/s1001-20260704.kqr.zst");
        assert_eq!(sv.ts, "2026-07-04T14:00:01+08:00");
    }

    #[test]
    fn parses_verify_failed() {
        let text = format!("{PREFIX}kairos-record-verify: /data/s1001.kqr.zst FAILED: bad header\n");
        let sv = parse_ship_verify(&text).unwrap();
        assert!(!sv.ok);
        assert!(sv.detail.contains("bad header"));
    }

    #[test]
    fn parses_ship_verify_failed() {
        let text = format!("{PREFIX}kairos-record-ship: VERIFY FAILED: /data/s1002.kqr.zst\n");
        let sv = parse_ship_verify(&text).unwrap();
        assert!(!sv.ok);
        assert_eq!(sv.detail, "/data/s1002.kqr.zst");
    }

    #[test]
    fn parses_ship_done_and_latest_wins() {
        let text = format!(
            "{PREFIX}kairos-record-ship: VERIFY FAILED: /data/s1002.kqr.zst\n\
             {PREFIX}kairos-record-ship: done (20260704)\n"
        );
        let sv = parse_ship_verify(&text).unwrap();
        assert!(sv.ok);
        assert_eq!(sv.detail, "done (20260704)");
    }

    #[test]
    fn no_ship_lines_is_none() {
        let text = format!("{PREFIX}kairos-record-ship: starting run\n-- No entries --\n");
        assert!(parse_ship_verify(&text).is_none());
    }
}
