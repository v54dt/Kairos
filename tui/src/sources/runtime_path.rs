//! Shared runtime-path resolver mirroring the cross-language contract pinned by
//! `schema/testdata/runtime_paths.txt`: an explicit env value (non-empty) wins
//! verbatim, else `$XDG_RUNTIME_DIR` joined `dir/base`, else `/run/user/<uid>`
//! when that dir exists, else `None`. The halt sentinel, hub status file and
//! scenario control socket all differ only by their `base` name.

use std::path::{Path, PathBuf};

pub(crate) fn resolve(
    explicit: Option<&str>,
    xdg: Option<&str>,
    run_user: Option<&str>,
    base: &str,
) -> Option<String> {
    if let Some(p) = explicit
        && !p.is_empty()
    {
        return Some(p.to_string());
    }
    if let Some(dir) = xdg
        && !dir.is_empty()
    {
        return Some(format!("{dir}/{base}"));
    }
    if let Some(dir) = run_user
        && !dir.is_empty()
    {
        return Some(format!("{dir}/{base}"));
    }
    None
}

pub(crate) fn run_user_dir() -> Option<String> {
    // SAFETY: getuid() is infallible and has no preconditions.
    let dir = format!("/run/user/{}", unsafe { libc::getuid() });
    Path::new(&dir).is_dir().then_some(dir)
}

/// Resolve a runtime path for `base`, reading the explicit override from
/// `env_key` and `$XDG_RUNTIME_DIR`, falling back to `/run/user/<uid>`.
pub(crate) fn path(env_key: &str, base: &str) -> Option<PathBuf> {
    resolve(
        std::env::var(env_key).ok().as_deref(),
        std::env::var("XDG_RUNTIME_DIR").ok().as_deref(),
        run_user_dir().as_deref(),
        base,
    )
    .map(PathBuf::from)
}
