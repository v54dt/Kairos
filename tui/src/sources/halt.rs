use std::fs::OpenOptions;
use std::io::ErrorKind;
use std::path::{Path, PathBuf};

// Admin-halt kill switch. The order hub (B5) checks a sentinel file's EXISTENCE
// on every submit and re-publishes `halted` each ~2s cycle; this module writes
// and removes that file. Its path MUST match the hub's `HubHaltPath()` exactly,
// or the switch writes a file the hub never reads.

/// Halt sentinel path, mirroring the C++ `HubHaltPath()` resolution byte-for-byte:
/// `$KAIROS_HUB_HALT`, else `$XDG_RUNTIME_DIR/kairos-hub-halt`, else
/// `/run/user/<uid>/kairos-hub-halt` (only if that dir exists), else `None`
/// (kill switch unavailable).
pub fn hub_halt_path() -> Option<PathBuf> {
    super::runtime_path::path("KAIROS_HUB_HALT", "kairos-hub-halt")
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
/// confirmation completes, the authorized action. Thin adapter over the shared
/// typed-buffer sub-machine: `HALT` arms, `RESUME` clears.
pub fn handle_key(prompt: &HaltPrompt, key: HaltKey) -> (HaltPrompt, Option<HaltAction>) {
    use crate::sources::confirm::{Transition, typed_step};
    match prompt {
        HaltPrompt::Idle => (HaltPrompt::Idle, None),
        HaltPrompt::ConfirmHalt(buf) => match typed_step(buf, "HALT", key) {
            Transition::Edit(b) => (HaltPrompt::ConfirmHalt(b), None),
            Transition::Resolve(fire) => (HaltPrompt::Idle, fire.then_some(HaltAction::Arm)),
        },
        HaltPrompt::ConfirmResume(buf) => match typed_step(buf, "RESUME", key) {
            Transition::Edit(b) => (HaltPrompt::ConfirmResume(b), None),
            Transition::Resolve(fire) => (HaltPrompt::Idle, fire.then_some(HaltAction::Clear)),
        },
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

    use crate::sources::runtime_path::resolve;

    const BASE: &str = "kairos-hub-halt";

    #[test]
    fn resolver_matches_hub_halt_path() {
        // Explicit env wins verbatim (the hub returns the same string).
        assert_eq!(
            resolve(
                Some("/scratch/kairos-hub-halt"),
                Some("/run/user/1001"),
                None,
                BASE
            ),
            Some("/scratch/kairos-hub-halt".to_string())
        );
        // Only $XDG_RUNTIME_DIR -> $XDG/kairos-hub-halt.
        assert_eq!(
            resolve(None, Some("/run/user/1001"), Some("/run/user/1001"), BASE),
            Some("/run/user/1001/kairos-hub-halt".to_string())
        );
        // /run/user/<uid> fallback.
        assert_eq!(
            resolve(None, None, Some("/run/user/1001"), BASE),
            Some("/run/user/1001/kairos-hub-halt".to_string())
        );
        // Disabled: no runtime dir.
        assert_eq!(resolve(None, None, None, BASE), None);
        assert_eq!(resolve(Some(""), Some(""), Some(""), BASE), None);
    }

    // Shared cross-language golden: rows for this module's base must resolve the
    // same here as in core resolve() and exec ResolveSock.
    #[test]
    fn golden_runtime_paths_hub_halt() {
        let fixture = include_str!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../schema/testdata/runtime_paths.txt"
        ));
        fn token(t: &str) -> Option<&str> {
            match t {
                "UNSET" => None,
                "EMPTY" => Some(""),
                v => Some(v),
            }
        }
        let mut rows = 0;
        for line in fixture.lines() {
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let f: Vec<&str> = line.split('|').collect();
            assert_eq!(f.len(), 5, "bad row: {line}");
            if f[3] != "kairos-hub-halt" {
                continue;
            }
            let ru = match f[2] {
                "yes" => Some("/run/user/1000"),
                "no" => None,
                other => panic!("bad run_user: {other}"),
            };
            let got = resolve(token(f[0]), token(f[1]), ru, BASE);
            let want = (f[4] != "FATAL").then(|| f[4].to_string());
            assert_eq!(got, want, "row: {line}");
            rows += 1;
        }
        assert!(rows >= 8, "no rows for kairos-hub-halt: {rows}");
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
        let resolved = resolve(None, Some(dir.to_str().unwrap()), None, BASE).unwrap();
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
