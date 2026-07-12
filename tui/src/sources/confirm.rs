//! One typed-buffer / y-N confirm sub-machine shared by the three real-money
//! confirm prompts (halt, service, scenario). It operates on the buffer alone;
//! each domain enum keeps its own shape and adapts onto these two functions.

use crate::sources::halt::HaltKey;

/// The outcome of feeding one key into a typed-buffer confirm: keep prompting
/// with the edited buffer, or resolve (leave the prompt) firing iff the bool.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Transition {
    Edit(String),
    Resolve(bool),
}

/// Advance a typed-buffer confirm. Enter fires only on an exact, non-empty match
/// of `buf` to `target` (case- and whitespace-sensitive); Cancel (Esc) resolves
/// without firing; Backspace/Char edit the buffer. The empty-target guard makes
/// a bare Enter against an empty required word never fire.
pub fn typed_step(buf: &str, target: &str, key: HaltKey) -> Transition {
    match key {
        HaltKey::Cancel => Transition::Resolve(false),
        HaltKey::Enter => Transition::Resolve(buf == target && !target.is_empty()),
        HaltKey::Backspace => {
            let mut b = buf.to_string();
            b.pop();
            Transition::Edit(b)
        }
        HaltKey::Char(c) => {
            let mut b = buf.to_string();
            b.push(c);
            Transition::Edit(b)
        }
    }
}

/// A single-key y/N confirm: fires only on 'y'/'Y'; anything else does not.
pub fn simple_step(key: HaltKey) -> bool {
    matches!(key, HaltKey::Char('y') | HaltKey::Char('Y'))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn empty_target_never_fires() {
        assert_eq!(
            typed_step("", "", HaltKey::Enter),
            Transition::Resolve(false)
        );
        assert_eq!(
            typed_step("x", "", HaltKey::Enter),
            Transition::Resolve(false)
        );
    }

    #[test]
    fn enter_fires_only_on_exact_match() {
        assert_eq!(
            typed_step("HALT", "HALT", HaltKey::Enter),
            Transition::Resolve(true)
        );
        // Case-sensitive.
        assert_eq!(
            typed_step("halt", "HALT", HaltKey::Enter),
            Transition::Resolve(false)
        );
        // Whitespace-sensitive.
        assert_eq!(
            typed_step("HALT ", "HALT", HaltKey::Enter),
            Transition::Resolve(false)
        );
        assert_eq!(
            typed_step("HAL", "HALT", HaltKey::Enter),
            Transition::Resolve(false)
        );
    }

    #[test]
    fn cancel_resolves_without_firing() {
        assert_eq!(
            typed_step("HALT", "HALT", HaltKey::Cancel),
            Transition::Resolve(false)
        );
    }

    #[test]
    fn char_and_backspace_edit_the_buffer() {
        assert_eq!(
            typed_step("HA", "HALT", HaltKey::Char('L')),
            Transition::Edit("HAL".to_string())
        );
        assert_eq!(
            typed_step("HAL", "HALT", HaltKey::Backspace),
            Transition::Edit("HA".to_string())
        );
        // Backspace on an empty buffer stays empty.
        assert_eq!(
            typed_step("", "HALT", HaltKey::Backspace),
            Transition::Edit(String::new())
        );
    }

    #[test]
    fn simple_step_fires_only_on_y() {
        assert!(simple_step(HaltKey::Char('y')));
        assert!(simple_step(HaltKey::Char('Y')));
        for key in [
            HaltKey::Char('n'),
            HaltKey::Char('x'),
            HaltKey::Enter,
            HaltKey::Cancel,
            HaltKey::Backspace,
        ] {
            assert!(!simple_step(key), "{key:?} must not fire");
        }
    }
}
