//! Archive-safety lens: refusal edge cases for replayd's live-dir guard.

use kairos_core::ipc::aeron::resolve_aeron_dir;
use kairos_core::replay::{default_aeron_dir, refuses_live_dir};

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

/// DOCUMENTED LIMITATION (not a bug): `$KAIROS_AERON_DIR` is the replay-isolation
/// escape hatch a replay environment sets to point the whole stack at a private dir,
/// so replayd deliberately does NOT treat it as the live dir — refusing it would
/// break isolated replays. The live-dir refusal keys off the native default only;
/// archive safety when production sets `$KAIROS_AERON_DIR` is covered by the marker
/// (recordd refuses while a replay owns that dir).
#[test]
fn kairos_aeron_dir_is_not_treated_as_the_live_dir() {
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
    // replayd's live-dir notion is the native default, not KAIROS_AERON_DIR:
    assert_eq!(default_aeron_dir().as_deref(), Some("/dev/shm/aeron-bob"));
    assert!(
        !refuses_live_dir("/dev/shm/aeron-live", default_aeron_dir().as_deref(), false),
        "KAIROS_AERON_DIR is the isolation escape hatch; refusing it would break replays"
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
