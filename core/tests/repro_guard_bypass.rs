//! REGRESSION (archive-safety lens). replayd's live-dir refusal and recordd's marker
//! guard both key off `default_aeron_dir()`. It used to read only `$USER` and return
//! `None` when unset, silently no-opping both guards even though Aeron still uses a
//! real live dir (/dev/shm/aeron-<getpwuid>). It now resolves the same name Aeron
//! does, so the guards name the real live dir with `$USER` unset. Single test fn: env
//! mutation is process-global, so everything runs sequentially here.

use kairos_core::ipc::aeron::resolve_aeron_dir;
use kairos_core::replay::marker::marker_path;
use kairos_core::replay::{default_aeron_dir, ensure_no_active_replay, refuses_live_dir};

#[test]
fn user_unset_still_names_the_live_dir() {
    let saved_user = std::env::var("USER").ok();
    let saved_kad = std::env::var("KAIROS_AERON_DIR").ok();
    let saved_ad = std::env::var("AERON_DIR").ok();
    unsafe {
        std::env::remove_var("USER");
        std::env::remove_var("KAIROS_AERON_DIR");
        std::env::remove_var("AERON_DIR");
    }

    // The dir the media driver actually uses on Linux with USER unset is
    // /dev/shm/aeron-<getpwuid name>; take the current OS user for the assertion.
    let osuser = String::from_utf8(
        std::process::Command::new("id")
            .arg("-un")
            .output()
            .unwrap()
            .stdout,
    )
    .unwrap();
    let live_dir = format!("/dev/shm/aeron-{}", osuser.trim());

    // (1) replayd side: default_aeron_dir() now resolves the real live dir via
    //     getpwuid, so refuses_live_dir fires and replayd refuses the live feed.
    assert_eq!(
        default_aeron_dir().as_deref(),
        Some(live_dir.as_str()),
        "USER unset -> guard still names the real live dir"
    );
    assert!(
        refuses_live_dir(&live_dir, default_aeron_dir().as_deref(), false),
        "replayd must refuse the real live dir {live_dir} when USER is unset"
    );

    // (2) recordd side: its guard dir is resolve_aeron_dir(None).or_else(default_aeron_dir);
    //     with USER + KAIROS_AERON_DIR unset it now resolves the real live dir, so
    //     ensure_no_active_replay is called on the dir recordd actually records from.
    let guard_dir = resolve_aeron_dir(None).or_else(default_aeron_dir);
    assert_eq!(
        guard_dir.as_deref(),
        Some(live_dir.as_str()),
        "recordd's marker check now targets the real live dir"
    );

    // Plant a live marker (this process's own pid = alive) in the real live dir and
    // show that the guard dir recordd checks would now refuse.
    let _ = std::fs::create_dir_all(&live_dir);
    let mpath = marker_path(&live_dir);
    let pre_existing = mpath.exists();
    if !pre_existing {
        std::fs::write(&mpath, std::process::id().to_string()).unwrap();
    }
    assert!(
        ensure_no_active_replay(guard_dir.as_deref().unwrap()).is_err(),
        "recordd refuses to record while a live replay owns the effective dir"
    );
    if !pre_existing {
        let _ = std::fs::remove_file(&mpath);
    }

    unsafe {
        match saved_user {
            Some(u) => std::env::set_var("USER", u),
            None => std::env::remove_var("USER"),
        }
        if let Some(k) = saved_kad {
            std::env::set_var("KAIROS_AERON_DIR", k);
        }
        if let Some(d) = saved_ad {
            std::env::set_var("AERON_DIR", d);
        }
    }
}
