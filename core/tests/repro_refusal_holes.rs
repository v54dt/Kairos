//! Archive-safety lens: refusal edge cases for replayd's live-dir guard.

use kairos_core::ipc::aeron::resolve_aeron_dir;
use kairos_core::replay::{effective_stack_dir, refuses_live_dir};

/// REGRESSION: a non-canonical spelling of the live default (symlink/`.`/`//`/`..`)
/// is the same physical dir and must be refused. The guard now compares canonical
/// identity instead of only trimming trailing slashes.
#[test]
fn noncanonical_path_is_refused() {
    let live = "/dev/shm/aeron-coder";
    for alias in [
        "/dev/shm/aeron-coder/.",
        "/dev/shm//aeron-coder",
        "/dev/shm/../shm/aeron-coder",
    ] {
        assert!(
            refuses_live_dir(alias, Some(live), false),
            "{alias} aliases the live dir and must be refused"
        );
    }
}

/// REGRESSION (archive-safety): when the live stack is pointed at an isolated dir via
/// `$KAIROS_AERON_DIR`, THAT is the dir driver/core/recordd actually use, so replayd
/// must treat it as the live dir and refuse to replay onto it. The refusal now keys off
/// the effective stack dir (KAIROS_AERON_DIR-aware), not the native default alone.
/// `--force-live-dir` stays the escape hatch for an intentional replay onto that dir.
#[test]
fn kairos_aeron_dir_live_stack_is_refused() {
    let saved_user = std::env::var("USER").ok();
    let saved_kad = std::env::var("KAIROS_AERON_DIR").ok();
    let saved_ad = std::env::var("AERON_DIR").ok();
    unsafe {
        std::env::set_var("USER", "bob");
        std::env::set_var("KAIROS_AERON_DIR", "/dev/shm/aeron-live");
        std::env::remove_var("AERON_DIR");
    }
    // The dir the live driver/core actually use via resolve_aeron_dir:
    assert_eq!(
        resolve_aeron_dir(None).as_deref(),
        Some("/dev/shm/aeron-live")
    );
    // replayd's refusal target is now that same effective stack dir, not aeron-bob:
    let target = effective_stack_dir(None);
    assert_eq!(target.as_deref(), Some("/dev/shm/aeron-live"));
    assert!(
        refuses_live_dir("/dev/shm/aeron-live", target.as_deref(), false),
        "the KAIROS_AERON_DIR live dir must be refused"
    );
    assert!(
        !refuses_live_dir("/dev/shm/aeron-live", target.as_deref(), true),
        "--force-live-dir remains the override"
    );
    unsafe {
        match saved_user {
            Some(u) => std::env::set_var("USER", u),
            None => std::env::remove_var("USER"),
        }
        match saved_kad {
            Some(k) => std::env::set_var("KAIROS_AERON_DIR", k),
            None => std::env::remove_var("KAIROS_AERON_DIR"),
        }
        match saved_ad {
            Some(d) => std::env::set_var("AERON_DIR", d),
            None => std::env::remove_var("AERON_DIR"),
        }
    }
}
