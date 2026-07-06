//! Scenario-panel confirm/action layer. The supervisor daemon owns spawning,
//! reaping, and running-detection; this module is only the keyboard state machine
//! that turns a keystroke into a guarded start/stop request. Mode is explicit per
//! key ('s' paper, 'l' live, 't' test), so a live start is only ever produced by
//! typing the scenario name — the sole real-money gate.

use crate::sources::halt::HaltKey;
use crate::sources::supervisor::Mode;

/// An action authorized by a completed confirmation. `Start` carries the mode
/// captured at confirm time, so a paper/test confirm can never emit a live start.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ScenarioAction {
    Start { name: String, mode: Mode },
    Stop { name: String },
}

/// Confirmation prompt state. A LIVE start requires typing the scenario name then
/// Enter (the sole real-money gate); a PAPER/TEST start and any STOP are a single
/// y/N. `Idle` means nothing is pending. Each variant binds the exact target name
/// shown so the operator cannot confirm a different scenario.
#[derive(Clone, Debug, PartialEq, Eq, Default)]
pub enum ScenarioPrompt {
    #[default]
    Idle,
    TypedStart {
        name: String,
        buf: String,
    },
    SimpleStart {
        name: String,
        mode: Mode,
    },
    SimpleStop {
        name: String,
        live: bool,
    },
}

impl ScenarioPrompt {
    pub fn is_active(&self) -> bool {
        !matches!(self, ScenarioPrompt::Idle)
    }
}

/// Open a PAPER start confirmation (y/N).
pub fn begin_start_paper(name: &str) -> ScenarioPrompt {
    ScenarioPrompt::SimpleStart {
        name: name.to_string(),
        mode: Mode::Paper,
    }
}

/// Open a TEST start confirmation (y/N). The daemon turns test mode into
/// `--ignore-window` so a scenario can be exercised off-hours.
pub fn begin_start_test(name: &str) -> ScenarioPrompt {
    ScenarioPrompt::SimpleStart {
        name: name.to_string(),
        mode: Mode::Test,
    }
}

/// Open a LIVE start confirmation: the operator must type the scenario name and
/// Enter. An empty name yields no startable prompt (`Idle`), so a bare Enter can
/// never confirm a LIVE start against an empty required buffer.
pub fn begin_start_live(name: &str) -> ScenarioPrompt {
    if name.is_empty() {
        return ScenarioPrompt::Idle;
    }
    ScenarioPrompt::TypedStart {
        name: name.to_string(),
        buf: String::new(),
    }
}

/// Open a stop confirmation. A stop only ever reduces exposure, so it is y/N.
pub fn begin_stop(name: &str, live: bool) -> ScenarioPrompt {
    ScenarioPrompt::SimpleStop {
        name: name.to_string(),
        live,
    }
}

/// Advance the confirm state machine. A typed start fires only on Enter with the
/// buffer equal to the scenario name; a simple start/stop fires only on 'y'/'Y'.
/// Anything else, or Cancel (Esc), returns to `Idle` without acting.
pub fn handle_key(
    prompt: &ScenarioPrompt,
    key: HaltKey,
) -> (ScenarioPrompt, Option<ScenarioAction>) {
    match prompt {
        ScenarioPrompt::Idle => (ScenarioPrompt::Idle, None),
        ScenarioPrompt::TypedStart { name, buf } => match key {
            HaltKey::Cancel => (ScenarioPrompt::Idle, None),
            HaltKey::Enter => {
                if buf == name && !name.is_empty() {
                    (
                        ScenarioPrompt::Idle,
                        Some(ScenarioAction::Start {
                            name: name.clone(),
                            mode: Mode::Live,
                        }),
                    )
                } else {
                    (ScenarioPrompt::Idle, None)
                }
            }
            HaltKey::Backspace => {
                let mut b = buf.clone();
                b.pop();
                (rebuild_typed(name, b), None)
            }
            HaltKey::Char(c) => {
                let mut b = buf.clone();
                b.push(c);
                (rebuild_typed(name, b), None)
            }
        },
        ScenarioPrompt::SimpleStart { name, mode } => match key {
            HaltKey::Char('y') | HaltKey::Char('Y') => (
                ScenarioPrompt::Idle,
                Some(ScenarioAction::Start {
                    name: name.clone(),
                    mode: *mode,
                }),
            ),
            _ => (ScenarioPrompt::Idle, None),
        },
        ScenarioPrompt::SimpleStop { name, .. } => match key {
            HaltKey::Char('y') | HaltKey::Char('Y') => (
                ScenarioPrompt::Idle,
                Some(ScenarioAction::Stop { name: name.clone() }),
            ),
            _ => (ScenarioPrompt::Idle, None),
        },
    }
}

fn rebuild_typed(name: &str, buf: String) -> ScenarioPrompt {
    ScenarioPrompt::TypedStart {
        name: name.to_string(),
        buf,
    }
}

/// Read-only state the Scenarios panel needs to render the confirm flow. `sel` is
/// a single cursor over the supervisor snapshot rows.
#[derive(Clone, Debug, Default)]
pub struct ScenarioUi {
    pub sel: usize,
    pub confirm: ScenarioPrompt,
    pub last_result: Option<String>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sources::supervisor::start_request;

    fn feed(mut p: ScenarioPrompt, s: &str) -> ScenarioPrompt {
        for c in s.chars() {
            p = handle_key(&p, HaltKey::Char(c)).0;
        }
        p
    }

    #[test]
    fn paper_start_is_simple_confirm() {
        assert_eq!(
            begin_start_paper("0050"),
            ScenarioPrompt::SimpleStart {
                name: "0050".to_string(),
                mode: Mode::Paper,
            }
        );
    }

    #[test]
    fn test_start_is_simple_confirm() {
        assert_eq!(
            begin_start_test("0050"),
            ScenarioPrompt::SimpleStart {
                name: "0050".to_string(),
                mode: Mode::Test,
            }
        );
    }

    #[test]
    fn live_start_is_typed_confirm() {
        assert!(matches!(
            begin_start_live("2330"),
            ScenarioPrompt::TypedStart { .. }
        ));
    }

    #[test]
    fn empty_name_live_start_is_not_startable() {
        let p = begin_start_live("");
        assert_eq!(p, ScenarioPrompt::Idle);
        let (next, action) = handle_key(&p, HaltKey::Enter);
        assert_eq!(
            action, None,
            "a bare Enter must never confirm an empty-name LIVE start"
        );
        assert_eq!(next, ScenarioPrompt::Idle);
    }

    #[test]
    fn typed_start_fires_only_on_name_and_enter() {
        let p = feed(begin_start_live("2330"), "2330");
        let (next, action) = handle_key(&p, HaltKey::Enter);
        assert_eq!(
            action,
            Some(ScenarioAction::Start {
                name: "2330".to_string(),
                mode: Mode::Live,
            })
        );
        assert_eq!(next, ScenarioPrompt::Idle);
    }

    #[test]
    fn typed_start_wrong_buffer_cancels_no_action() {
        let p = feed(begin_start_live("2330"), "233");
        let (next, action) = handle_key(&p, HaltKey::Enter);
        assert_eq!(action, None);
        assert_eq!(next, ScenarioPrompt::Idle);
    }

    #[test]
    fn typed_start_esc_cancels_no_action() {
        let p = feed(begin_start_live("2330"), "2330");
        let (next, action) = handle_key(&p, HaltKey::Cancel);
        assert_eq!(action, None);
        assert_eq!(next, ScenarioPrompt::Idle);
    }

    #[test]
    fn typed_start_backspace_edits() {
        let p = feed(begin_start_live("2330"), "23X");
        match handle_key(&p, HaltKey::Backspace).0 {
            ScenarioPrompt::TypedStart { buf, .. } => assert_eq!(buf, "23"),
            other => panic!("expected TypedStart, got {other:?}"),
        }
    }

    #[test]
    fn paper_start_fires_only_on_y() {
        for c in ['y', 'Y'] {
            let (next, action) = handle_key(&begin_start_paper("0050"), HaltKey::Char(c));
            assert_eq!(
                action,
                Some(ScenarioAction::Start {
                    name: "0050".to_string(),
                    mode: Mode::Paper,
                })
            );
            assert_eq!(next, ScenarioPrompt::Idle);
        }
        for key in [
            HaltKey::Char('n'),
            HaltKey::Char('x'),
            HaltKey::Enter,
            HaltKey::Cancel,
        ] {
            let (next, action) = handle_key(&begin_start_paper("0050"), key);
            assert_eq!(action, None, "{key:?} must not start a paper trader");
            assert_eq!(next, ScenarioPrompt::Idle);
        }
    }

    #[test]
    fn test_start_fires_only_on_y_and_carries_test_mode() {
        let (_, action) = handle_key(&begin_start_test("0050"), HaltKey::Char('y'));
        assert_eq!(
            action,
            Some(ScenarioAction::Start {
                name: "0050".to_string(),
                mode: Mode::Test,
            })
        );
    }

    #[test]
    fn stop_is_simple_confirm_and_fires_only_on_y() {
        assert!(matches!(
            begin_stop("2330", true),
            ScenarioPrompt::SimpleStop { .. }
        ));
        let (next, action) = handle_key(&begin_stop("2330", true), HaltKey::Char('y'));
        assert_eq!(
            action,
            Some(ScenarioAction::Stop {
                name: "2330".to_string()
            })
        );
        assert_eq!(next, ScenarioPrompt::Idle);
        for key in [HaltKey::Char('n'), HaltKey::Enter, HaltKey::Cancel] {
            let (_, action) = handle_key(&begin_stop("2330", true), key);
            assert_eq!(action, None, "{key:?} must not stop");
        }
    }

    #[test]
    fn idle_ignores_keys() {
        let (next, action) = handle_key(&ScenarioPrompt::Idle, HaltKey::Char('y'));
        assert_eq!(next, ScenarioPrompt::Idle);
        assert_eq!(action, None);
    }

    // The whole point of the redesign: no key sequence other than the typed-name
    // path can yield a Mode::Live action, and only that action stamps "live".
    #[test]
    fn no_single_key_path_yields_a_live_start() {
        // Paper and test simple starts on 'y' never produce Live.
        for begin in [begin_start_paper("2330"), begin_start_test("2330")] {
            let (_, action) = handle_key(&begin, HaltKey::Char('y'));
            match action {
                Some(ScenarioAction::Start { mode, .. }) => assert_ne!(mode, Mode::Live),
                other => panic!("unexpected action {other:?}"),
            }
        }
        // The typed path with a WRONG buffer never fires a live start.
        let wrong = feed(begin_start_live("2330"), "0050");
        assert_eq!(handle_key(&wrong, HaltKey::Enter).1, None);
    }

    #[test]
    fn start_request_stamps_live_only_from_live_mode() {
        assert!(start_request("2330", Mode::Live).contains("\"mode\":\"live\""));
        assert!(!start_request("2330", Mode::Paper).contains("live"));
        assert!(!start_request("2330", Mode::Test).contains("live"));
    }
}
