//! Process orchestration for kairos-sim: locate the daemon binaries, spawn each
//! into its own process group, and tear the whole subtree down on drop (normal
//! exit, Ctrl-C, SIGTERM, or panic unwind) so no sim daemon is ever orphaned.

use std::io;
use std::path::{Path, PathBuf};
use std::process::{Child as StdChild, Command};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use crate::sim::paths::SimPaths;

/// The Aeron control file the media driver creates once it is up.
pub const CNC_FILE: &str = "cnc.dat";

/// A spawned daemon, tracked by name for teardown/status messages.
pub struct Spawned {
    pub name: String,
    child: StdChild,
}

impl Spawned {
    /// Spawn `cmd` in a fresh process group (so a killpg reaches grandchildren).
    pub fn start(name: &str, mut cmd: Command) -> io::Result<Self> {
        use std::os::unix::process::CommandExt;
        // SAFETY: setpgid(0, 0) only reassigns this child's process group; it has no
        // preconditions and touches no shared state before exec.
        unsafe {
            cmd.pre_exec(|| {
                if libc::setpgid(0, 0) != 0 {
                    return Err(io::Error::last_os_error());
                }
                Ok(())
            });
        }
        let child = cmd.spawn()?;
        Ok(Self {
            name: name.to_owned(),
            child,
        })
    }

    fn pgid(&self) -> libc::pid_t {
        self.child.id() as libc::pid_t
    }
}

/// Owns every spawned daemon and kills all of them (SIGTERM, then SIGKILL after a
/// grace) on drop. Drop runs on the normal return path, on the signal-observed
/// return, and during panic unwinding, so there is no path that leaks children.
pub struct ChildGuard {
    children: Vec<Spawned>,
    grace: Duration,
}

impl ChildGuard {
    pub fn new() -> Self {
        Self {
            children: Vec::new(),
            grace: Duration::from_secs(3),
        }
    }

    pub fn push(&mut self, child: Spawned) {
        self.children.push(child);
    }

    pub fn is_empty(&self) -> bool {
        self.children.is_empty()
    }

    /// Names of children that have already exited on their own (e.g. a daemon that
    /// died at startup), so the launcher can fail loudly instead of hanging.
    pub fn exited(&mut self) -> Vec<String> {
        self.children
            .iter_mut()
            .filter_map(|s| matches!(s.child.try_wait(), Ok(Some(_))).then(|| s.name.clone()))
            .collect()
    }

    /// (name, process-group id) for every spawned child, for the pidfile a separate
    /// `down`/`status` invocation reads.
    pub fn pgids(&self) -> Vec<(String, libc::pid_t)> {
        self.children
            .iter()
            .map(|s| (s.name.clone(), s.pgid()))
            .collect()
    }
}

impl Default for ChildGuard {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for ChildGuard {
    fn drop(&mut self) {
        for s in &self.children {
            // SAFETY: killpg with a valid pgid only signals that group.
            unsafe { libc::killpg(s.pgid(), libc::SIGTERM) };
        }
        let deadline = Instant::now() + self.grace;
        loop {
            let all_gone = self
                .children
                .iter_mut()
                .all(|s| matches!(s.child.try_wait(), Ok(Some(_))));
            if all_gone || Instant::now() >= deadline {
                break;
            }
            std::thread::sleep(Duration::from_millis(20));
        }
        for s in &mut self.children {
            if matches!(s.child.try_wait(), Ok(None)) {
                // SAFETY: same as above; force-kill a group that ignored SIGTERM.
                unsafe { libc::killpg(s.pgid(), libc::SIGKILL) };
            }
            let _ = s.child.wait();
        }
    }
}

/// `dir/name` if it exists as a file, else a clear error (never silently proceed).
pub fn locate_bin(dir: &Path, name: &str) -> anyhow::Result<PathBuf> {
    let p = dir.join(name);
    if p.is_file() {
        Ok(p)
    } else {
        anyhow::bail!(
            "required binary '{name}' not found at {}; build it or pass an override",
            p.display()
        )
    }
}

/// Dir holding kairos-sim's sibling Rust daemons (driver/core/replayd): the
/// directory of the current executable.
pub fn sibling_bin_dir(current_exe: &Path) -> anyhow::Result<PathBuf> {
    current_exe
        .parent()
        .map(Path::to_path_buf)
        .ok_or_else(|| anyhow::anyhow!("cannot determine the kairos-sim binary directory"))
}

/// Locate kairos_sim_hubd: an explicit override wins, else walk `current_exe`'s
/// ancestors for `exec/scenario/build/kairos_sim_hubd`. Clear error if none found.
pub fn resolve_hubd(override_path: Option<&str>, current_exe: &Path) -> anyhow::Result<PathBuf> {
    if let Some(p) = override_path.filter(|s| !s.is_empty()) {
        let p = PathBuf::from(p);
        if p.is_file() {
            return Ok(p);
        }
        anyhow::bail!(
            "kairos_sim_hubd not found at {} (from override)",
            p.display()
        );
    }
    for anc in current_exe.ancestors() {
        let cand = anc.join("exec/scenario/build/kairos_sim_hubd");
        if cand.is_file() {
            return Ok(cand);
        }
    }
    anyhow::bail!(
        "kairos_sim_hubd not found under any ancestor of {}; pass --hubd or set $KAIROS_SIM_HUBD",
        current_exe.display()
    )
}

/// Outcome of `wait_ready`: the pipeline came up, or a signal was observed during
/// bring-up (the caller must tear down without proceeding).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Ready {
    Up,
    Interrupted,
}

/// Block until the sim pipeline is attachable — the driver's `cnc.dat` and both
/// sockets exist — or the timeout elapses. Errors if any child has already died.
/// Returns `Ready::Interrupted` if `stop` is set (a signal arrived during bring-up)
/// so the caller unwinds and the `ChildGuard` reaps every child.
pub fn wait_ready(
    paths: &SimPaths,
    guard: &mut ChildGuard,
    stop: &AtomicBool,
    timeout: Duration,
) -> anyhow::Result<Ready> {
    let cnc = Path::new(&paths.aeron_dir).join(CNC_FILE);
    let deadline = Instant::now() + timeout;
    loop {
        if stop.load(Ordering::Relaxed) {
            return Ok(Ready::Interrupted);
        }
        let dead = guard.exited();
        if !dead.is_empty() {
            anyhow::bail!("sim daemon(s) exited during startup: {}", dead.join(", "));
        }
        if cnc.exists()
            && Path::new(&paths.quote_sock).exists()
            && Path::new(&paths.order_sock).exists()
        {
            return Ok(Ready::Up);
        }
        if Instant::now() >= deadline {
            anyhow::bail!(
                "sim pipeline not ready within {:?}: waiting on {} + {} + {}",
                timeout,
                cnc.display(),
                paths.quote_sock,
                paths.order_sock
            );
        }
        std::thread::sleep(Duration::from_millis(50));
    }
}

/// Expand replay sources into a concrete KQR file list for kairos-replayd: a
/// directory contributes its `*.kqr` files (sorted for a stable merge tie-break); a
/// file passes through. Errors on a missing path or a directory holding no KQR file,
/// so `kairos-sim replay <FILE|DIR>...` matches what it advertises.
pub fn resolve_kqr_sources(sources: &[PathBuf]) -> anyhow::Result<Vec<PathBuf>> {
    let mut files = Vec::new();
    for src in sources {
        let meta = std::fs::metadata(src)
            .map_err(|e| anyhow::anyhow!("cannot access replay source {}: {e}", src.display()))?;
        if meta.is_dir() {
            let mut found: Vec<PathBuf> = std::fs::read_dir(src)?
                .filter_map(|e| e.ok().map(|e| e.path()))
                .filter(|p| p.is_file() && p.extension().and_then(|e| e.to_str()) == Some("kqr"))
                .collect();
            found.sort();
            if found.is_empty() {
                anyhow::bail!("no .kqr files in directory {}", src.display());
            }
            files.extend(found);
        } else {
            files.push(src.clone());
        }
    }
    Ok(files)
}

/// Sidecar file (outside the Aeron dir, so the driver's delete-on-start never
/// clears it) recording the running sim's process groups for `down`/`status`.
pub fn pidfile_path(paths: &SimPaths) -> PathBuf {
    PathBuf::from(format!("{}.kairos-sim.pids", paths.aeron_dir))
}

pub fn write_pidfile(paths: &SimPaths, entries: &[(String, libc::pid_t)]) -> io::Result<()> {
    let body: String = entries
        .iter()
        .map(|(name, pgid)| format!("{pgid} {name}\n"))
        .collect();
    std::fs::write(pidfile_path(paths), body)
}

/// Parse the pidfile into (pgid, name) pairs; missing/garbage -> empty.
pub fn read_pidfile(paths: &SimPaths) -> Vec<(libc::pid_t, String)> {
    let Ok(text) = std::fs::read_to_string(pidfile_path(paths)) else {
        return Vec::new();
    };
    text.lines()
        .filter_map(|line| {
            let (pgid, name) = line.split_once(' ')?;
            Some((pgid.trim().parse().ok()?, name.trim().to_owned()))
        })
        .collect()
}

pub fn remove_pidfile(paths: &SimPaths) {
    let _ = std::fs::remove_file(pidfile_path(paths));
}

/// True if the process group still exists (`killpg(pgid, 0)` succeeds or EPERM).
pub fn pgid_alive(pgid: libc::pid_t) -> bool {
    if pgid <= 0 {
        return false;
    }
    // SAFETY: signal 0 performs only an existence/permission check.
    let rc = unsafe { libc::killpg(pgid, 0) };
    rc == 0 || io::Error::last_os_error().raw_os_error() == Some(libc::EPERM)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(tag: &str) -> PathBuf {
        let p =
            std::env::temp_dir().join(format!("kairos-sim-proc-{}-{}", std::process::id(), tag));
        let _ = std::fs::remove_dir_all(&p);
        std::fs::create_dir_all(&p).unwrap();
        p
    }

    #[test]
    fn locate_bin_present_and_missing() {
        let dir = tmp("locate");
        std::fs::write(dir.join("kairos-driver"), b"#!/bin/true").unwrap();
        assert!(locate_bin(&dir, "kairos-driver").is_ok());
        let err = locate_bin(&dir, "kairos-core").unwrap_err().to_string();
        assert!(err.contains("kairos-core"), "{err}");
    }

    #[test]
    fn resolve_hubd_walks_ancestors() {
        let root = tmp("hubd");
        let build = root.join("exec/scenario/build");
        std::fs::create_dir_all(&build).unwrap();
        std::fs::write(build.join("kairos_sim_hubd"), b"x").unwrap();
        // Pretend kairos-sim lives deep in the Rust target tree under the same root.
        let exe = root.join("core/target/debug/kairos-sim");
        std::fs::create_dir_all(exe.parent().unwrap()).unwrap();
        let found = resolve_hubd(None, &exe).unwrap();
        assert_eq!(found, build.join("kairos_sim_hubd"));
    }

    #[test]
    fn resolve_hubd_override_missing_errors() {
        let exe = PathBuf::from("/nonexistent/kairos-sim");
        assert!(resolve_hubd(Some("/no/such/hubd"), &exe).is_err());
        assert!(resolve_hubd(None, &exe).is_err());
    }

    #[test]
    fn pidfile_round_trips() {
        let dir = tmp("pidfile");
        let paths = SimPaths {
            aeron_dir: dir.join("aeron-sim").to_string_lossy().into_owned(),
            quote_sock: "/x/q.sock".into(),
            order_sock: "/x/o.sock".into(),
        };
        assert!(read_pidfile(&paths).is_empty());
        write_pidfile(
            &paths,
            &[("kairos-driver".into(), 111), ("kairos-core".into(), 222)],
        )
        .unwrap();
        let got = read_pidfile(&paths);
        assert_eq!(
            got,
            vec![
                (111, "kairos-driver".to_owned()),
                (222, "kairos-core".to_owned())
            ]
        );
        remove_pidfile(&paths);
        assert!(read_pidfile(&paths).is_empty());
    }

    #[test]
    fn resolve_kqr_sources_expands_dir_and_passes_files() {
        let dir = tmp("kqr-src");
        for f in ["s1002-x.kqr", "s1001-x.kqr", "notes.txt"] {
            std::fs::write(dir.join(f), b"x").unwrap();
        }
        // A directory expands to its sorted *.kqr files, skipping non-KQR entries.
        let got = resolve_kqr_sources(std::slice::from_ref(&dir)).unwrap();
        assert_eq!(
            got,
            vec![dir.join("s1001-x.kqr"), dir.join("s1002-x.kqr")]
        );
        // An explicit file passes through; multiple sources concatenate in order.
        let file = dir.join("s1001-x.kqr");
        let got = resolve_kqr_sources(&[file.clone(), dir.clone()]).unwrap();
        assert_eq!(
            got,
            vec![file.clone(), dir.join("s1001-x.kqr"), dir.join("s1002-x.kqr")]
        );
    }

    #[test]
    fn resolve_kqr_sources_errors_on_missing_and_empty_dir() {
        let dir = tmp("kqr-empty");
        assert!(resolve_kqr_sources(std::slice::from_ref(&dir)).is_err());
        assert!(resolve_kqr_sources(&[dir.join("nope.kqr")]).is_err());
    }

    #[test]
    fn guard_reaps_a_child() {
        let mut guard = ChildGuard::new();
        let mut cmd = Command::new("sleep");
        cmd.arg("30");
        let child = Spawned::start("sleep", cmd).unwrap();
        let pgid = child.pgid();
        guard.push(child);
        drop(guard);
        // The group is gone: killpg returns ESRCH.
        let rc = unsafe { libc::killpg(pgid, 0) };
        assert_eq!(rc, -1, "process group should no longer exist");
    }
}
