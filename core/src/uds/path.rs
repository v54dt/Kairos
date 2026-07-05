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
    if let Some(d) = std::env::var("XDG_RUNTIME_DIR")
        .ok()
        .filter(|d| !d.is_empty())
    {
        return Some(d);
    }
    run_user_dir()
}

/// Quote UDS path: `$KAIROS_QUOTE_SOCK`, else `$XDG_RUNTIME_DIR/kairos-quotes.sock`,
/// else `/run/user/<uid>/kairos-quotes.sock`. Never /tmp (world-writable); exits if
/// none resolves. C++/other consumers follow the same rule.
pub fn quote_socket_path() -> String {
    resolve(
        std::env::var("KAIROS_QUOTE_SOCK").ok().as_deref(),
        std::env::var("XDG_RUNTIME_DIR").ok().as_deref(),
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
        std::env::var("KAIROS_ORDER_SOCK").ok().as_deref(),
        std::env::var("XDG_RUNTIME_DIR").ok().as_deref(),
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
}
