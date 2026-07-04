//! Replay/record mutual exclusion. A running replay drops a marker file into its
//! target Aeron dir; kairos-recordd refuses to start when a live marker is present
//! in ITS Aeron dir, so a replay can never pollute the real archive. This is the
//! enforcement mechanism — not a doc-only convention. Also holds the pure helpers
//! that keep a fat-fingered replay out of the live default dir.

use std::path::{Path, PathBuf};

use crate::ipc::aeron::resolve_aeron_dir;

/// Marker file name written into the target Aeron dir by a running replay.
pub const MARKER_NAME: &str = "kairos-replay.marker";

/// Path of the replay marker inside `aeron_dir`.
pub fn marker_path(aeron_dir: &str) -> PathBuf {
    Path::new(aeron_dir).join(MARKER_NAME)
}

/// Removes the replay marker it wrote when dropped (clean exit or Ctrl-C).
pub struct MarkerGuard {
    path: PathBuf,
}

impl Drop for MarkerGuard {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.path);
    }
}

/// Write the replay marker (this process's pid) into `aeron_dir`, creating the dir
/// if needed. The returned guard removes it on drop.
pub fn write_marker(aeron_dir: &str) -> std::io::Result<MarkerGuard> {
    std::fs::create_dir_all(aeron_dir)?;
    let path = marker_path(aeron_dir);
    std::fs::write(&path, std::process::id().to_string())?;
    Ok(MarkerGuard { path })
}

/// True if a process with `pid` currently exists. `kill(pid, 0)` succeeds for a
/// live process and fails with `EPERM` for one we may not signal (still alive);
/// only `ESRCH` (no such process) counts as dead.
pub fn pid_is_alive(pid: i32) -> bool {
    if pid <= 0 {
        return false;
    }
    // SAFETY: kill with signal 0 performs only an existence/permission check.
    let rc = unsafe { libc::kill(pid, 0) };
    if rc == 0 {
        return true;
    }
    std::io::Error::last_os_error().raw_os_error() == Some(libc::EPERM)
}

/// Refuse to start recording when an active replay marker sits in `aeron_dir`.
/// Absent -> Ok. Present with a live pid -> hard error. Present with a dead pid
/// (a crashed replay left it behind) -> warn, remove, and continue.
pub fn ensure_no_active_replay(aeron_dir: &str) -> anyhow::Result<()> {
    let path = marker_path(aeron_dir);
    let contents = match std::fs::read_to_string(&path) {
        Ok(c) => c,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(()),
        Err(e) => anyhow::bail!("cannot read replay marker {}: {e}", path.display()),
    };
    match contents.trim().parse::<i32>() {
        Ok(pid) if pid_is_alive(pid) => anyhow::bail!(
            "a replay is active (pid {pid}) on this Aeron dir; refusing to record and pollute the \
             archive. Stop the replay or remove {}",
            path.display()
        ),
        Ok(_) => {
            eprintln!(
                "kairos-recordd: stale replay marker at {} (replay gone); removing",
                path.display()
            );
            let _ = std::fs::remove_file(&path);
            Ok(())
        }
        Err(_) => anyhow::bail!(
            "unreadable replay marker at {}; remove it manually if no replay is running",
            path.display()
        ),
    }
}

/// The Aeron dir the live stack falls back to when no dir is given explicitly,
/// mirroring the media driver's own resolution: `$AERON_DIR` if set, else
/// `/dev/shm/aeron-<user>`. The user name follows Aeron's `username()`: `$USER`,
/// then `getpwuid`, then `"default"` — so the guard still names the real live dir
/// when `$USER` is unset (systemd/cron/container), where Aeron uses `getpwuid`.
pub fn default_aeron_dir() -> Option<String> {
    if let Some(d) = std::env::var("AERON_DIR").ok().filter(|d| !d.is_empty()) {
        return Some(d);
    }
    Some(format!("/dev/shm/aeron-{}", current_username()))
}

/// The Aeron dir the live stack (driver/core/recordd) actually resolves to for a
/// given `--aeron-dir`: an explicit dir or `$KAIROS_AERON_DIR` (via `resolve_aeron_dir`),
/// else the native default (`$AERON_DIR`, else `/dev/shm/aeron-<user>`). This is the
/// single notion of "the live dir" shared by replayd's refusal and recordd's marker
/// check, so a guard can never watch a different dir than the stack uses.
pub fn effective_stack_dir(explicit: Option<&str>) -> Option<String> {
    resolve_aeron_dir(explicit).or_else(default_aeron_dir)
}

/// The current user name, matching Aeron's `aeron_username()`: `$USER` first, then
/// the passwd entry for the real uid, then the literal `"default"`.
fn current_username() -> String {
    if let Some(u) = std::env::var("USER").ok().filter(|u| !u.is_empty()) {
        return u;
    }
    // SAFETY: getpwuid returns a pointer into a static buffer valid until the next
    // libc password call; pw_name is copied out immediately.
    unsafe {
        let pw = libc::getpwuid(libc::getuid());
        if pw.is_null() || (*pw).pw_name.is_null() {
            return "default".to_owned();
        }
        std::ffi::CStr::from_ptr((*pw).pw_name)
            .to_str()
            .ok()
            .filter(|s| !s.is_empty())
            .map_or_else(|| "default".to_owned(), str::to_owned)
    }
}

/// True if `target` and the `live` stack dir are the same physical directory and
/// `--force-live-dir` was not passed. Paths are compared by canonical identity, so
/// symlinks and non-canonical spellings (`/x/.`, `//x`, `/y/../x`) don't slip past.
pub fn refuses_live_dir(target: &str, live: Option<&str>, force: bool) -> bool {
    if force {
        return false;
    }
    match live {
        Some(d) => dir_key(target) == dir_key(d),
        None => false,
    }
}

/// Canonical identity of a path for same-dir comparison: `canonicalize` when the
/// path exists (resolving symlinks), else a lexical normalization that folds `.`,
/// `..`, and repeated separators. A live default and its aliases resolve equal.
fn dir_key(s: &str) -> PathBuf {
    let p = Path::new(s);
    std::fs::canonicalize(p).unwrap_or_else(|_| lexical_normalize(p))
}

fn lexical_normalize(p: &Path) -> PathBuf {
    use std::path::Component;
    let mut out = PathBuf::new();
    for comp in p.components() {
        match comp {
            Component::CurDir => {}
            Component::ParentDir => {
                out.pop();
            }
            other => out.push(other.as_os_str()),
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(tag: &str) -> String {
        let p = std::env::temp_dir().join(format!("kairos-marker-{}-{}", std::process::id(), tag));
        let _ = std::fs::remove_dir_all(&p);
        std::fs::create_dir_all(&p).unwrap();
        p.to_string_lossy().into_owned()
    }

    #[test]
    fn absent_marker_is_ok() {
        let dir = tmp("absent");
        assert!(ensure_no_active_replay(&dir).is_ok());
    }

    #[test]
    fn live_marker_refuses() {
        let dir = tmp("live");
        std::fs::write(marker_path(&dir), std::process::id().to_string()).unwrap();
        assert!(ensure_no_active_replay(&dir).is_err());
        // Still present (not removed for a live replay).
        assert!(marker_path(&dir).exists());
    }

    #[test]
    fn stale_marker_is_removed_and_ok() {
        let dir = tmp("stale");
        // A pid that is almost certainly not running.
        std::fs::write(marker_path(&dir), "2147483000").unwrap();
        assert!(ensure_no_active_replay(&dir).is_ok());
        assert!(!marker_path(&dir).exists());
    }

    #[test]
    fn unreadable_marker_refuses() {
        let dir = tmp("garbage");
        std::fs::write(marker_path(&dir), "not-a-pid").unwrap();
        assert!(ensure_no_active_replay(&dir).is_err());
    }

    #[test]
    fn write_marker_guard_removes_on_drop() {
        let dir = tmp("guard");
        {
            let _g = write_marker(&dir).unwrap();
            assert!(marker_path(&dir).exists());
        }
        assert!(!marker_path(&dir).exists());
    }

    #[test]
    fn self_pid_is_alive() {
        assert!(pid_is_alive(std::process::id() as i32));
        assert!(!pid_is_alive(2_147_483_000));
        assert!(!pid_is_alive(0));
    }

    #[test]
    fn refuses_live_dir_logic() {
        assert!(refuses_live_dir(
            "/dev/shm/aeron-bob",
            Some("/dev/shm/aeron-bob"),
            false
        ));
        // Trailing slash normalized.
        assert!(refuses_live_dir(
            "/dev/shm/aeron-bob/",
            Some("/dev/shm/aeron-bob"),
            false
        ));
        // --force-live-dir overrides.
        assert!(!refuses_live_dir(
            "/dev/shm/aeron-bob",
            Some("/dev/shm/aeron-bob"),
            true
        ));
        // Different dir is fine.
        assert!(!refuses_live_dir(
            "/tmp/replay",
            Some("/dev/shm/aeron-bob"),
            false
        ));
        // Unknown default never refuses.
        assert!(!refuses_live_dir("/dev/shm/aeron-bob", None, false));
    }
}
