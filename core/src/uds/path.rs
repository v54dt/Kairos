fn resolve(
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

/// `/run/user/<uid>` (the XDG runtime dir) if it exists, even when `$XDG_RUNTIME_DIR`
/// is not exported. It is per-user mode 0700, so the socket stays private.
fn run_user_dir() -> Option<String> {
    // SAFETY: getuid() is infallible and has no preconditions.
    let dir = format!("/run/user/{}", unsafe { libc::getuid() });
    std::path::Path::new(&dir).is_dir().then_some(dir)
}

/// The per-user runtime dir the sockets live in: `$XDG_RUNTIME_DIR` if non-empty,
/// else `/run/user/<uid>` when it exists. `None` if neither is usable. Used by the
/// sim launcher to name its own namespaced sockets under the same private dir.
pub fn runtime_dir() -> Option<String> {
    if let Some(d) = crate::config::env_nonempty("XDG_RUNTIME_DIR") {
        return Some(d);
    }
    run_user_dir()
}

/// Quote UDS path: `$KAIROS_QUOTE_SOCK`, else `$XDG_RUNTIME_DIR/kairos-quotes.sock`,
/// else `/run/user/<uid>/kairos-quotes.sock`. Never /tmp (world-writable); exits if
/// none resolves. C++/other consumers follow the same rule.
pub fn quote_socket_path() -> String {
    resolve(
        crate::config::env_nonempty("KAIROS_QUOTE_SOCK").as_deref(),
        crate::config::env_nonempty("XDG_RUNTIME_DIR").as_deref(),
        run_user_dir().as_deref(),
        "kairos-quotes.sock",
    )
    .unwrap_or_else(|| {
        eprintln!(
            "kairos: no runtime dir for the quote socket; set $KAIROS_QUOTE_SOCK or $XDG_RUNTIME_DIR"
        );
        std::process::exit(1);
    })
}

/// Order-hub UDS path: `$KAIROS_ORDER_SOCK`, else `$XDG_RUNTIME_DIR/kairos-orders.sock`,
/// else `/run/user/<uid>/kairos-orders.sock`. Mirrors the C++ `OrderSocketPath()`; this
/// is the LIVE order socket the sim isolation guard must never collide with.
pub fn order_socket_path() -> String {
    resolve(
        crate::config::env_nonempty("KAIROS_ORDER_SOCK").as_deref(),
        crate::config::env_nonempty("XDG_RUNTIME_DIR").as_deref(),
        run_user_dir().as_deref(),
        "kairos-orders.sock",
    )
    .unwrap_or_else(|| {
        eprintln!(
            "kairos: no runtime dir for the order socket; set $KAIROS_ORDER_SOCK or $XDG_RUNTIME_DIR"
        );
        std::process::exit(1);
    })
}

#[cfg(test)]
mod tests {
    use super::resolve;

    const Q: &str = "kairos-quotes.sock";

    #[test]
    fn explicit_override_wins() {
        assert_eq!(
            resolve(Some("/run/k.sock"), Some("/run/user/1001"), None, Q),
            Some("/run/k.sock".to_string())
        );
    }

    #[test]
    fn xdg_runtime_dir_preferred() {
        assert_eq!(
            resolve(None, Some("/run/user/1001"), Some("/run/user/1001"), Q),
            Some("/run/user/1001/kairos-quotes.sock".to_string())
        );
    }

    #[test]
    fn run_user_when_no_xdg() {
        assert_eq!(
            resolve(None, None, Some("/run/user/1001"), Q),
            Some("/run/user/1001/kairos-quotes.sock".to_string())
        );
    }

    #[test]
    fn base_name_is_used() {
        assert_eq!(
            resolve(None, Some("/run/user/1001"), None, "kairos-orders.sock"),
            Some("/run/user/1001/kairos-orders.sock".to_string())
        );
    }

    #[test]
    fn none_when_nothing_usable() {
        assert_eq!(resolve(None, None, None, Q), None);
        assert_eq!(resolve(Some(""), Some(""), Some(""), Q), None);
    }

    #[test]
    fn empty_values_skip_to_next() {
        assert_eq!(
            resolve(Some(""), None, Some("/run/user/1001"), Q),
            Some("/run/user/1001/kairos-quotes.sock".to_string())
        );
    }

    // Shared cross-language golden: every row of schema/testdata/runtime_paths.txt
    // must resolve identically here, in exec ResolveSock, and in the tui resolvers.
    const RUNTIME_PATHS: &str = include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../schema/testdata/runtime_paths.txt"
    ));

    fn env_token(tok: &str) -> Option<&str> {
        match tok {
            "UNSET" => None,
            "EMPTY" => Some(""),
            v => Some(v),
        }
    }

    #[test]
    fn golden_runtime_paths_all_rows_match() {
        let mut rows = 0;
        for line in RUNTIME_PATHS.lines() {
            let line = line.trim_end();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let f: Vec<&str> = line.split('|').collect();
            assert_eq!(f.len(), 5, "bad row: {line}");
            let (env, xdg, run_user, base, expected) = (f[0], f[1], f[2], f[3], f[4]);
            let ru = match run_user {
                "yes" => Some("/run/user/1000"),
                "no" => None,
                other => panic!("bad run_user: {other}"),
            };
            let got = resolve(env_token(env), env_token(xdg), ru, base);
            let want = (expected != "FATAL").then(|| expected.to_string());
            assert_eq!(got, want, "row: {line}");
            rows += 1;
        }
        assert!(rows >= 40, "fixture shrank unexpectedly: {rows} rows");
    }
}
