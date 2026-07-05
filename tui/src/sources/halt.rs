use std::fs::OpenOptions;
use std::io::ErrorKind;
use std::path::{Path, PathBuf};

// Admin-halt kill switch. The order hub (B5) checks a sentinel file's EXISTENCE
// on every submit and re-publishes `halted` each ~2s cycle; this module writes
// and removes that file. Its path MUST match the hub's `HubHaltPath()` exactly,
// or the switch writes a file the hub never reads.

fn resolve(explicit: Option<&str>, xdg: Option<&str>, run_user: Option<&str>) -> Option<String> {
    if let Some(p) = explicit
        && !p.is_empty()
    {
        return Some(p.to_string());
    }
    if let Some(dir) = xdg
        && !dir.is_empty()
    {
        return Some(format!("{dir}/kairos-hub-halt"));
    }
    if let Some(dir) = run_user
        && !dir.is_empty()
    {
        return Some(format!("{dir}/kairos-hub-halt"));
    }
    None
}

fn run_user_dir() -> Option<String> {
    // SAFETY: getuid() is infallible and has no preconditions.
    let dir = format!("/run/user/{}", unsafe { libc::getuid() });
    Path::new(&dir).is_dir().then_some(dir)
}

/// Halt sentinel path, mirroring the C++ `HubHaltPath()` resolution byte-for-byte:
/// `$KAIROS_HUB_HALT`, else `$XDG_RUNTIME_DIR/kairos-hub-halt`, else
/// `/run/user/<uid>/kairos-hub-halt` (only if that dir exists), else `None`
/// (kill switch unavailable).
pub fn hub_halt_path() -> Option<PathBuf> {
    resolve(
        std::env::var("KAIROS_HUB_HALT").ok().as_deref(),
        std::env::var("XDG_RUNTIME_DIR").ok().as_deref(),
        run_user_dir().as_deref(),
    )
    .map(PathBuf::from)
}

/// Atomically create the halt sentinel (O_CREAT|O_EXCL, no partial-file window).
/// The hub only checks existence, so content is irrelevant; an already-present
/// file is the desired end-state and reported as success.
pub fn arm_halt(path: &Path) -> Result<(), String> {
    match OpenOptions::new().write(true).create_new(true).open(path) {
        Ok(_) => Ok(()),
        Err(e) if e.kind() == ErrorKind::AlreadyExists => Ok(()),
        Err(e) => Err(e.to_string()),
    }
}

/// Remove the halt sentinel to resume. An already-absent file is success.
pub fn clear_halt(path: &Path) -> Result<(), String> {
    match std::fs::remove_file(path) {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == ErrorKind::NotFound => Ok(()),
        Err(e) => Err(e.to_string()),
    }
}

/// A single keystroke fed into the confirm state machine.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HaltKey {
    Char(char),
    Enter,
    Backspace,
    Cancel,
}

/// The action a completed confirmation authorizes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HaltAction {
    Arm,
    Clear,
}

/// Typed-confirmation prompt state. Arming requires the literal word `HALT`;
/// resuming requires `RESUME` (both exact case) plus Enter. Anything else, or
/// Cancel (Esc), returns to `Idle` without acting.
#[derive(Clone, Debug, PartialEq, Eq, Default)]
pub enum HaltPrompt {
    #[default]
    Idle,
    ConfirmHalt(String),
    ConfirmResume(String),
}

impl HaltPrompt {
    pub fn is_active(&self) -> bool {
        !matches!(self, HaltPrompt::Idle)
    }
}

/// Advance the confirm state machine. Returns the next prompt state and, when a
/// confirmation completes, the authorized action.
pub fn handle_key(prompt: &HaltPrompt, key: HaltKey) -> (HaltPrompt, Option<HaltAction>) {
    match prompt {
        HaltPrompt::Idle => (HaltPrompt::Idle, None),
        HaltPrompt::ConfirmHalt(buf) => step(buf, key, "HALT", HaltAction::Arm, |b| {
            HaltPrompt::ConfirmHalt(b)
        }),
        HaltPrompt::ConfirmResume(buf) => step(buf, key, "RESUME", HaltAction::Clear, |b| {
            HaltPrompt::ConfirmResume(b)
        }),
    }
}

fn step(
    buf: &str,
    key: HaltKey,
    word: &str,
    action: HaltAction,
    rebuild: impl Fn(String) -> HaltPrompt,
) -> (HaltPrompt, Option<HaltAction>) {
    match key {
        HaltKey::Cancel => (HaltPrompt::Idle, None),
        HaltKey::Enter => {
            if buf == word {
                (HaltPrompt::Idle, Some(action))
            } else {
                (HaltPrompt::Idle, None)
            }
        }
        HaltKey::Backspace => {
            let mut b = buf.to_string();
            b.pop();
            (rebuild(b), None)
        }
        HaltKey::Char(c) => {
            let mut b = buf.to_string();
            b.push(c);
            (rebuild(b), None)
        }
    }
}

/// Read-only state the Risk panel needs to render the kill switch.
pub struct HaltUi {
    pub path: Option<PathBuf>,
    pub prompt: HaltPrompt,
    pub last_result: Option<String>,
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp_path(tag: &str) -> PathBuf {
        std::env::temp_dir().join(format!("kairos-halt-{}-{}", std::process::id(), tag))
    }

    #[test]
    fn resolver_matches_hub_halt_path() {
        // Explicit env wins verbatim (the hub returns the same string).
        assert_eq!(
            resolve(
                Some("/scratch/kairos-hub-halt"),
                Some("/run/user/1001"),
                None
            ),
            Some("/scratch/kairos-hub-halt".to_string())
        );
        // Only $XDG_RUNTIME_DIR -> $XDG/kairos-hub-halt.
        assert_eq!(
            resolve(None, Some("/run/user/1001"), Some("/run/user/1001")),
            Some("/run/user/1001/kairos-hub-halt".to_string())
        );
        // /run/user/<uid> fallback.
        assert_eq!(
            resolve(None, None, Some("/run/user/1001")),
            Some("/run/user/1001/kairos-hub-halt".to_string())
        );
        // Disabled: no runtime dir.
        assert_eq!(resolve(None, None, None), None);
        assert_eq!(resolve(Some(""), Some(""), Some("")), None);
    }

    #[test]
    fn typing_halt_then_enter_arms() {
        let mut p = HaltPrompt::ConfirmHalt(String::new());
        for c in "HALT".chars() {
            p = handle_key(&p, HaltKey::Char(c)).0;
        }
        let (next, action) = handle_key(&p, HaltKey::Enter);
        assert_eq!(action, Some(HaltAction::Arm));
        assert_eq!(next, HaltPrompt::Idle);
    }

    #[test]
    fn wrong_buffer_enter_cancels() {
        let p = HaltPrompt::ConfirmHalt("halt".to_string());
        let (next, action) = handle_key(&p, HaltKey::Enter);
        assert_eq!(action, None);
        assert_eq!(next, HaltPrompt::Idle);
    }

    #[test]
    fn esc_cancels_without_action() {
        let p = HaltPrompt::ConfirmHalt("HALT".to_string());
        let (next, action) = handle_key(&p, HaltKey::Cancel);
        assert_eq!(action, None);
        assert_eq!(next, HaltPrompt::Idle);
    }

    #[test]
    fn backspace_edits_buffer() {
        let p = HaltPrompt::ConfirmHalt("HALX".to_string());
        let (next, _) = handle_key(&p, HaltKey::Backspace);
        assert_eq!(next, HaltPrompt::ConfirmHalt("HAL".to_string()));
    }

    #[test]
    fn resume_then_enter_clears() {
        let mut p = HaltPrompt::ConfirmResume(String::new());
        for c in "RESUME".chars() {
            p = handle_key(&p, HaltKey::Char(c)).0;
        }
        let (next, action) = handle_key(&p, HaltKey::Enter);
        assert_eq!(action, Some(HaltAction::Clear));
        assert_eq!(next, HaltPrompt::Idle);
    }

    #[test]
    fn idle_ignores_keys() {
        let (next, action) = handle_key(&HaltPrompt::Idle, HaltKey::Char('x'));
        assert_eq!(next, HaltPrompt::Idle);
        assert_eq!(action, None);
    }

    #[test]
    fn arm_creates_sentinel_and_is_idempotent() {
        let path = tmp_path("arm");
        let _ = std::fs::remove_file(&path);
        assert!(!path.exists());
        assert!(arm_halt(&path).is_ok());
        assert!(path.exists());
        // Second arm on an existing file is the desired end-state: Ok.
        assert!(arm_halt(&path).is_ok());
        assert!(path.exists());
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn clear_removes_sentinel_and_is_idempotent() {
        let path = tmp_path("clear");
        arm_halt(&path).unwrap();
        assert!(path.exists());
        assert!(clear_halt(&path).is_ok());
        assert!(!path.exists());
        // Clearing an absent file is Ok.
        assert!(clear_halt(&path).is_ok());
        assert!(!path.exists());
    }

    #[test]
    fn e2e_confirm_drives_sentinel_at_resolved_path() {
        // Point at a tempdir "runtime dir"; the resolved path is exactly what
        // the hub's existence check would use.
        let dir = std::env::temp_dir().join(format!("kairos-halt-e2e-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let resolved = resolve(None, Some(dir.to_str().unwrap()), None).unwrap();
        let path = PathBuf::from(&resolved);
        assert_eq!(path, dir.join("kairos-hub-halt"));
        let _ = std::fs::remove_file(&path);

        // Arm via the state machine, then apply.
        let mut p = HaltPrompt::ConfirmHalt(String::new());
        for c in "HALT".chars() {
            p = handle_key(&p, HaltKey::Char(c)).0;
        }
        let (_, action) = handle_key(&p, HaltKey::Enter);
        assert_eq!(action, Some(HaltAction::Arm));
        arm_halt(&path).unwrap();
        assert!(path.exists());

        // Resume via the state machine, then apply.
        let mut p = HaltPrompt::ConfirmResume(String::new());
        for c in "RESUME".chars() {
            p = handle_key(&p, HaltKey::Char(c)).0;
        }
        let (_, action) = handle_key(&p, HaltKey::Enter);
        assert_eq!(action, Some(HaltAction::Clear));
        clear_halt(&path).unwrap();
        assert!(!path.exists());

        let _ = std::fs::remove_dir_all(&dir);
    }
}
