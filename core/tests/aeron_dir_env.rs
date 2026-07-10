//! Confirms the live stack (kairos-core / kairos-driver / kairos-recordd) resolves
//! its Aeron directory from the environment through one shared helper, so a single
//! `$KAIROS_AERON_DIR` points every daemon at the same dir with a sane default.
//! One serial test (own process) since it mutates process-global env vars.

use kairos_core::ipc::aeron::resolve_aeron_dir;
use kairos_core::replay::marker::{default_aeron_dir, effective_stack_dir};

fn clear() {
    unsafe {
        std::env::remove_var("KAIROS_AERON_DIR");
        std::env::remove_var("AERON_DIR");
    }
}

#[test]
fn aeron_dir_env_resolution_precedence() {
    clear();

    // 1. Explicit --aeron-dir wins over everything.
    unsafe { std::env::set_var("KAIROS_AERON_DIR", "/env/kairos") };
    unsafe { std::env::set_var("AERON_DIR", "/env/native") };
    assert_eq!(
        effective_stack_dir(Some("/explicit")).as_deref(),
        Some("/explicit")
    );

    // 2. No explicit -> $KAIROS_AERON_DIR beats the native $AERON_DIR/default.
    assert_eq!(effective_stack_dir(None).as_deref(), Some("/env/kairos"));

    // 3. An empty $KAIROS_AERON_DIR is ignored and falls through to $AERON_DIR.
    unsafe { std::env::set_var("KAIROS_AERON_DIR", "") };
    assert_eq!(effective_stack_dir(None).as_deref(), Some("/env/native"));
    assert_eq!(
        resolve_aeron_dir(None),
        None,
        "empty KAIROS is treated unset"
    );

    // 4. Neither var set -> the native /dev/shm/aeron-<user> default is used.
    clear();
    assert_eq!(effective_stack_dir(None), default_aeron_dir());
    assert!(
        effective_stack_dir(None)
            .unwrap()
            .starts_with("/dev/shm/aeron-")
    );

    clear();
}
