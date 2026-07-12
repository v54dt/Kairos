// Adversarial safety probe (reviewer-authored). Attacks the shared confirm core
// and the three adapters through the real public API, trying to make a
// real-money confirm fire easier than on main. All asserts encode "must NOT
// fire" expectations; a green run means the refactor did not weaken the gates.

use kairos_tui::sources::confirm::{Transition, simple_step, typed_step};
use kairos_tui::sources::halt::{self, HaltAction, HaltKey, HaltPrompt};
use kairos_tui::sources::scenario_ctl::{self, ScenarioAction};
use kairos_tui::sources::service::{self, Verb};
use kairos_tui::sources::supervisor::Mode;

fn feed_halt(mut p: HaltPrompt, s: &str) -> HaltPrompt {
    for c in s.chars() {
        p = halt::handle_key(&p, HaltKey::Char(c)).0;
    }
    p
}

// ---- core: typed_step ----------------------------------------------------

#[test]
fn core_empty_target_never_fires_any_key() {
    for key in [HaltKey::Enter, HaltKey::Backspace, HaltKey::Char('x')] {
        // empty buf + empty target
        assert_eq!(
            typed_step("", "", HaltKey::Enter),
            Transition::Resolve(false)
        );
        let _ = key;
    }
    // Backspacing an empty buf down to "" and Enter against "" must not fire.
    assert_eq!(
        typed_step("", "", HaltKey::Enter),
        Transition::Resolve(false)
    );
}

#[test]
fn core_trailing_space_target_is_exact() {
    // If a target carried a trailing space, only the exact spaced buffer fires.
    assert_eq!(
        typed_step("HALT", "HALT ", HaltKey::Enter),
        Transition::Resolve(false)
    );
    assert_eq!(
        typed_step("HALT ", "HALT ", HaltKey::Enter),
        Transition::Resolve(true)
    );
}

#[test]
fn core_esc_then_enter_cannot_fire() {
    // Esc resolves(false); a subsequent Enter is a fresh call and also cannot
    // fire because the machine returned to Idle in the adapter.
    assert_eq!(
        typed_step("HALT", "HALT", HaltKey::Cancel),
        Transition::Resolve(false)
    );
}

#[test]
fn core_simple_step_accepts_only_y_upper_lower() {
    assert!(simple_step(HaltKey::Char('y')));
    assert!(simple_step(HaltKey::Char('Y')));
    for k in [
        HaltKey::Enter,
        HaltKey::Cancel,
        HaltKey::Backspace,
        HaltKey::Char('n'),
        HaltKey::Char('N'),
        HaltKey::Char(' '),
        HaltKey::Char('\n'),
        HaltKey::Char('\r'),
        HaltKey::Char('1'),
    ] {
        assert!(!simple_step(k), "{k:?} must not fire y/N");
    }
}

// ---- halt: HALT / RESUME exact word --------------------------------------

#[test]
fn halt_lowercase_and_partial_and_padded_never_arm() {
    for bad in ["halt", "HAL", "HALT ", " HALT", "HALTT", "Halt", ""] {
        let p = HaltPrompt::ConfirmHalt(bad.to_string());
        assert_eq!(halt::handle_key(&p, HaltKey::Enter).1, None, "buf={bad:?}");
    }
    let good = feed_halt(HaltPrompt::ConfirmHalt(String::new()), "HALT");
    assert_eq!(
        halt::handle_key(&good, HaltKey::Enter).1,
        Some(HaltAction::Arm)
    );
}

#[test]
fn halt_backspace_to_match_still_requires_enter_not_char() {
    // Type HALTX, backspace to HALT, then a stray char must not arm; only Enter.
    let mut p = feed_halt(HaltPrompt::ConfirmHalt(String::new()), "HALTX");
    p = halt::handle_key(&p, HaltKey::Backspace).0; // -> HALT
    // A char keypress here appends, it never fires.
    assert_eq!(halt::handle_key(&p, HaltKey::Char('y')).1, None);
    // Enter on the exact HALT arms.
    assert_eq!(
        halt::handle_key(&p, HaltKey::Enter).1,
        Some(HaltAction::Arm)
    );
}

// ---- service: dangerous-unit stem match ----------------------------------

#[test]
fn service_dangerous_unit_stem_empty_degenerate_does_not_fire_on_bare_enter() {
    // ".service" has an EMPTY stem. On main the old code fired when buf==""==stem.
    // The core empty-guard makes this stricter (must not fire); verify it does
    // not fire, i.e. the refactor did not make it easier.
    let p = service::begin(Verb::Start, ".service");
    // Bare Enter with empty buffer.
    let (_, action) = service::handle_key(&p, HaltKey::Enter);
    assert_eq!(
        action, None,
        "empty-stem dangerous unit must not fire on bare Enter"
    );
}

#[test]
fn service_typed_confirm_requires_exact_stem() {
    let p0 = service::begin(Verb::Restart, "kairos-driver.service");
    for bad in [
        "kairos-drive",
        "kairos-driver ",
        "KAIROS-DRIVER",
        "kairos-driver.service",
    ] {
        let mut p = p0.clone();
        for c in bad.chars() {
            p = service::handle_key(&p, HaltKey::Char(c)).0;
        }
        assert_eq!(
            service::handle_key(&p, HaltKey::Enter).1,
            None,
            "buf={bad:?}"
        );
    }
}

#[test]
fn service_simple_confirm_only_y() {
    let p = service::begin(Verb::Start, "kairos-bsr.service");
    for k in [
        HaltKey::Char('n'),
        HaltKey::Enter,
        HaltKey::Char(' '),
        HaltKey::Char('Y'),
    ] {
        let fired = service::handle_key(&p, k).1.is_some();
        assert_eq!(fired, matches!(k, HaltKey::Char('Y')), "{k:?}");
    }
}

// ---- scenario: LIVE gate -- three fresh bypass attempts ------------------

fn live_fires(prompt: &scenario_ctl::ScenarioPrompt, key: HaltKey) -> bool {
    matches!(
        scenario_ctl::handle_key(prompt, key).1,
        Some(ScenarioAction::Start {
            mode: Mode::Live,
            ..
        })
    )
}

#[test]
fn bypass1_paper_confirm_then_reopen_never_yields_live() {
    // Attempt: open paper (y/N), press 'y' -> Paper action, never Live.
    let p = scenario_ctl::begin_start_paper("2330");
    assert!(!live_fires(&p, HaltKey::Char('y')));
    match scenario_ctl::handle_key(&p, HaltKey::Char('y')).1 {
        Some(ScenarioAction::Start { mode, .. }) => assert_ne!(mode, Mode::Live),
        other => panic!("unexpected {other:?}"),
    }
}

#[test]
fn bypass2_typed_live_with_whitespace_or_case_variants_never_fires_live() {
    let base = scenario_ctl::begin_start_live("2330");
    for bad in ["2330 ", " 2330", "233", "2330\n", "ABCD"] {
        let mut p = base.clone();
        for c in bad.chars() {
            p = scenario_ctl::handle_key(&p, HaltKey::Char(c)).0;
        }
        assert!(
            !live_fires(&p, HaltKey::Enter),
            "buf={bad:?} must not fire live"
        );
    }
    // Exact match DOES fire (control).
    let mut p = base.clone();
    for c in "2330".chars() {
        p = scenario_ctl::handle_key(&p, HaltKey::Char(c)).0;
    }
    assert!(live_fires(&p, HaltKey::Enter));
}

#[test]
fn bypass3_empty_name_live_even_if_typed_start_forced_never_fires() {
    // begin_start_live("") returns Idle; a bare Enter cannot fire. And even a
    // hand-forged TypedStart with an empty name cannot fire (core empty-guard).
    let idle = scenario_ctl::begin_start_live("");
    assert_eq!(idle, scenario_ctl::ScenarioPrompt::Idle);
    assert!(!live_fires(&idle, HaltKey::Enter));
    let forged = scenario_ctl::ScenarioPrompt::TypedStart {
        name: String::new(),
        buf: String::new(),
    };
    assert!(
        !live_fires(&forged, HaltKey::Enter),
        "empty-name forged TypedStart must not fire"
    );
}
