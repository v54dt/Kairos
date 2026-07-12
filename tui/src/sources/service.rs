use tokio::process::Command;

use crate::sources::halt::HaltKey;

/// A systemd management verb. Typed so a restart can never masquerade as a
/// reset-failed through the safe path.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Verb {
    Restart,
    Start,
    Stop,
    ResetFailed,
}

impl Verb {
    pub fn as_str(self) -> &'static str {
        match self {
            Verb::Restart => "restart",
            Verb::Start => "start",
            Verb::Stop => "stop",
            Verb::ResetFailed => "reset-failed",
        }
    }
}

/// Confirmation strength required before a management action fires.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Strength {
    Dangerous,
    Safe,
}

/// The unit name without its final `.service`/`.timer`/`.target` extension.
/// A name with no `.` is its own stem (kept whole for the fail-safe default).
fn unit_stem(unit: &str) -> &str {
    match unit.rsplit_once('.') {
        Some((stem, _)) => stem,
        None => unit,
    }
}

/// Explicit SAFE stems: research crons + harmless ops. Everything else is
/// DANGEROUS by fail-safe default.
const SAFE_STEMS: [&str; 5] = [
    "kairos-lab-ticks",
    "kairos-lab-ssf",
    "kairos-lab-chips",
    "kairos-lab-blacklist",
    "kairos-bsr",
];

/// Classify a management action. `reset-failed` only clears a status marker and
/// never touches a process, so it is SAFE for any unit. Otherwise the unit stem
/// must be in the explicit SAFE allowlist; an unknown/unclassified name defaults
/// to DANGEROUS (typed confirm), never the easy path.
pub fn classify(verb: Verb, unit: &str) -> Strength {
    if verb == Verb::ResetFailed {
        return Strength::Safe;
    }
    if SAFE_STEMS.contains(&unit_stem(unit)) {
        Strength::Safe
    } else {
        Strength::Dangerous
    }
}

/// The exact argv for a management action: `systemctl --user VERB UNIT`.
pub fn build_argv(verb: Verb, unit: &str) -> Vec<String> {
    vec![
        "systemctl".to_string(),
        "--user".to_string(),
        verb.as_str().to_string(),
        unit.to_string(),
    ]
}

/// Run `systemctl --user VERB UNIT`, capturing stdout+stderr+exit. Always
/// returns a human line (success or the error text); never propagates, so a
/// failure is surfaced inline rather than swallowed.
pub async fn run_action(verb: Verb, unit: &str) -> String {
    let argv = build_argv(verb, unit);
    let output = Command::new(&argv[0]).args(&argv[1..]).output().await;
    match output {
        Ok(o) if o.status.success() => format!("{} {} ok", verb.as_str(), unit),
        Ok(o) => {
            let mut msg = String::from_utf8_lossy(&o.stderr).trim().to_string();
            if msg.is_empty() {
                msg = String::from_utf8_lossy(&o.stdout).trim().to_string();
            }
            if msg.is_empty() {
                msg = format!("exited with {}", o.status);
            }
            format!("{} {} failed: {}", verb.as_str(), unit, msg)
        }
        Err(e) => format!("{} {} failed: {}", verb.as_str(), unit, e),
    }
}

/// A management action authorized by a completed confirmation. The only
/// constructor is [`begin`], so every action is routed through [`classify`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Action {
    pub verb: Verb,
    pub unit: String,
}

/// Confirmation prompt state. DANGEROUS targets require typing the unit stem
/// then Enter (binds the confirmation to the exact unit shown); SAFE targets
/// are a single y/N with default-N. `Idle` means no action is pending.
#[derive(Clone, Debug, PartialEq, Eq, Default)]
pub enum ConfirmPrompt {
    #[default]
    Idle,
    TypedConfirm {
        verb: Verb,
        unit: String,
        buf: String,
    },
    SimpleConfirm {
        verb: Verb,
        unit: String,
    },
}

impl ConfirmPrompt {
    pub fn is_active(&self) -> bool {
        !matches!(self, ConfirmPrompt::Idle)
    }
}

/// The single authorized entry point that opens a confirmation for an action.
/// It calls [`classify`] internally, so no action can bypass classification:
/// DANGEROUS -> typed confirm, SAFE -> y/N.
pub fn begin(verb: Verb, unit: &str) -> ConfirmPrompt {
    match classify(verb, unit) {
        Strength::Dangerous => ConfirmPrompt::TypedConfirm {
            verb,
            unit: unit.to_string(),
            buf: String::new(),
        },
        Strength::Safe => ConfirmPrompt::SimpleConfirm {
            verb,
            unit: unit.to_string(),
        },
    }
}

/// Advance the confirm state machine. Returns the next prompt and, when a
/// confirmation completes, the authorized action. Typed confirm fires only on
/// Enter with the buffer equal to the unit stem; simple confirm fires only on
/// 'y'/'Y'. Anything else, or Cancel (Esc), returns to `Idle` without acting.
pub fn handle_key(prompt: &ConfirmPrompt, key: HaltKey) -> (ConfirmPrompt, Option<Action>) {
    use crate::sources::confirm::{Transition, simple_step, typed_step};
    match prompt {
        ConfirmPrompt::Idle => (ConfirmPrompt::Idle, None),
        ConfirmPrompt::TypedConfirm { verb, unit, buf } => {
            match typed_step(buf, unit_stem(unit), key) {
                Transition::Edit(b) => (
                    ConfirmPrompt::TypedConfirm {
                        verb: *verb,
                        unit: unit.clone(),
                        buf: b,
                    },
                    None,
                ),
                Transition::Resolve(fire) => (
                    ConfirmPrompt::Idle,
                    fire.then(|| Action {
                        verb: *verb,
                        unit: unit.clone(),
                    }),
                ),
            }
        }
        ConfirmPrompt::SimpleConfirm { verb, unit } => (
            ConfirmPrompt::Idle,
            simple_step(key).then(|| Action {
                verb: *verb,
                unit: unit.clone(),
            }),
        ),
    }
}

/// Read-only state the Overview panel needs to render service management: the
/// selection cursor, the active confirmation, and the last action result.
#[derive(Clone, Debug, Default)]
pub struct ServiceUi {
    pub selected: usize,
    pub confirm: ConfirmPrompt,
    pub last_result: Option<String>,
}

#[cfg(test)]
mod tests {
    use super::*;

    const DANGEROUS_STEMS: [&str; 6] = [
        "kairos-driver",
        "kairos-core",
        "kairos-sidecar",
        "kairos-orderhub",
        "kairos-recordd",
        "kairos-record-ship",
    ];

    #[test]
    fn verb_strings_match_systemctl() {
        assert_eq!(Verb::Restart.as_str(), "restart");
        assert_eq!(Verb::Start.as_str(), "start");
        assert_eq!(Verb::Stop.as_str(), "stop");
        assert_eq!(Verb::ResetFailed.as_str(), "reset-failed");
    }

    #[test]
    fn classify_dangerous() {
        for stem in DANGEROUS_STEMS {
            for ext in [".service", ".timer"] {
                let unit = format!("{stem}{ext}");
                for verb in [Verb::Restart, Verb::Start, Verb::Stop] {
                    assert_eq!(
                        classify(verb, &unit),
                        Strength::Dangerous,
                        "{verb:?} {unit}"
                    );
                }
            }
        }
        for verb in [Verb::Restart, Verb::Start, Verb::Stop] {
            assert_eq!(classify(verb, "kairos.target"), Strength::Dangerous);
        }
    }

    #[test]
    fn classify_safe() {
        for stem in SAFE_STEMS {
            for ext in [".service", ".timer"] {
                let unit = format!("{stem}{ext}");
                for verb in [Verb::Restart, Verb::Start, Verb::Stop] {
                    assert_eq!(classify(verb, &unit), Strength::Safe, "{verb:?} {unit}");
                }
            }
        }
    }

    #[test]
    fn reset_failed_always_safe() {
        assert_eq!(
            classify(Verb::ResetFailed, "kairos-driver.service"),
            Strength::Safe
        );
        assert_eq!(classify(Verb::ResetFailed, "kairos.target"), Strength::Safe);
        assert_eq!(
            classify(Verb::ResetFailed, "totally-unknown.service"),
            Strength::Safe
        );
    }

    #[test]
    fn unknown_is_dangerous() {
        for unit in [
            "kairos-foo.service",
            "random.service",
            "kairos-driverx.service",
            "kairos-lab-ticksx.service",
            "kairos",
        ] {
            assert_eq!(
                classify(Verb::Start, unit),
                Strength::Dangerous,
                "{unit} must default dangerous"
            );
        }
    }

    #[test]
    fn build_argv_is_exact() {
        assert_eq!(
            build_argv(Verb::Restart, "kairos-driver.service"),
            vec!["systemctl", "--user", "restart", "kairos-driver.service"]
        );
        assert_eq!(
            build_argv(Verb::ResetFailed, "kairos-orderhub.service"),
            vec![
                "systemctl",
                "--user",
                "reset-failed",
                "kairos-orderhub.service"
            ]
        );
        assert_eq!(
            build_argv(Verb::Stop, "kairos.target"),
            vec!["systemctl", "--user", "stop", "kairos.target"]
        );
    }

    #[test]
    fn begin_picks_strength_by_target() {
        assert!(matches!(
            begin(Verb::Restart, "kairos-driver.service"),
            ConfirmPrompt::TypedConfirm { .. }
        ));
        assert!(matches!(
            begin(Verb::Stop, "kairos.target"),
            ConfirmPrompt::TypedConfirm { .. }
        ));
        assert!(matches!(
            begin(Verb::Start, "kairos-bsr.service"),
            ConfirmPrompt::SimpleConfirm { .. }
        ));
        assert!(matches!(
            begin(Verb::ResetFailed, "kairos-driver.service"),
            ConfirmPrompt::SimpleConfirm { .. }
        ));
        // Fail-safe: unknown unit gets the typed confirm.
        assert!(matches!(
            begin(Verb::Start, "kairos-foo.service"),
            ConfirmPrompt::TypedConfirm { .. }
        ));
    }

    fn feed(mut p: ConfirmPrompt, s: &str) -> ConfirmPrompt {
        for c in s.chars() {
            p = handle_key(&p, HaltKey::Char(c)).0;
        }
        p
    }

    #[test]
    fn typed_confirm_fires_on_exact_stem_and_enter() {
        let p = feed(
            begin(Verb::Restart, "kairos-driver.service"),
            "kairos-driver",
        );
        let (next, action) = handle_key(&p, HaltKey::Enter);
        assert_eq!(
            action,
            Some(Action {
                verb: Verb::Restart,
                unit: "kairos-driver.service".to_string(),
            })
        );
        assert_eq!(next, ConfirmPrompt::Idle);
    }

    #[test]
    fn typed_confirm_wrong_buffer_cancels() {
        let p = feed(begin(Verb::Stop, "kairos-core.service"), "kairos-cor");
        let (next, action) = handle_key(&p, HaltKey::Enter);
        assert_eq!(action, None);
        assert_eq!(next, ConfirmPrompt::Idle);
    }

    #[test]
    fn typed_confirm_esc_cancels() {
        let p = feed(begin(Verb::Stop, "kairos.target"), "kairos");
        let (next, action) = handle_key(&p, HaltKey::Cancel);
        assert_eq!(action, None);
        assert_eq!(next, ConfirmPrompt::Idle);
    }

    #[test]
    fn typed_confirm_backspace_edits() {
        let p = feed(begin(Verb::Restart, "kairos-driver.service"), "kairos-x");
        let p = handle_key(&p, HaltKey::Backspace).0;
        match p {
            ConfirmPrompt::TypedConfirm { buf, .. } => assert_eq!(buf, "kairos-"),
            other => panic!("expected TypedConfirm, got {other:?}"),
        }
    }

    #[test]
    fn simple_confirm_fires_only_on_y() {
        for c in ['y', 'Y'] {
            let p = begin(Verb::Start, "kairos-bsr.service");
            let (next, action) = handle_key(&p, HaltKey::Char(c));
            assert_eq!(
                action,
                Some(Action {
                    verb: Verb::Start,
                    unit: "kairos-bsr.service".to_string(),
                })
            );
            assert_eq!(next, ConfirmPrompt::Idle);
        }
        for key in [
            HaltKey::Char('n'),
            HaltKey::Char('x'),
            HaltKey::Enter,
            HaltKey::Cancel,
            HaltKey::Backspace,
        ] {
            let p = begin(Verb::Start, "kairos-bsr.service");
            let (next, action) = handle_key(&p, key);
            assert_eq!(action, None, "{key:?} must not fire");
            assert_eq!(next, ConfirmPrompt::Idle);
        }
    }

    #[test]
    fn idle_ignores_keys() {
        let (next, action) = handle_key(&ConfirmPrompt::Idle, HaltKey::Char('y'));
        assert_eq!(next, ConfirmPrompt::Idle);
        assert_eq!(action, None);
    }
}
