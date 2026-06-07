fn resolve(explicit: Option<&str>, xdg: Option<&str>) -> String {
    if let Some(p) = explicit
        && !p.is_empty()
    {
        return p.to_string();
    }
    if let Some(dir) = xdg
        && !dir.is_empty()
    {
        return format!("{dir}/kairos-quotes.sock");
    }
    "/tmp/kairos-quotes.sock".to_string()
}

/// Quote UDS path: `$KAIROS_QUOTE_SOCK`, else `$XDG_RUNTIME_DIR/kairos-quotes.sock`,
/// else `/tmp/kairos-quotes.sock`. Consumers in other languages follow the same rule.
pub fn quote_socket_path() -> String {
    resolve(
        std::env::var("KAIROS_QUOTE_SOCK").ok().as_deref(),
        std::env::var("XDG_RUNTIME_DIR").ok().as_deref(),
    )
}

#[cfg(test)]
mod tests {
    use super::resolve;

    #[test]
    fn explicit_override_wins() {
        assert_eq!(
            resolve(Some("/run/k.sock"), Some("/run/user/1001")),
            "/run/k.sock"
        );
    }

    #[test]
    fn xdg_runtime_dir_when_no_override() {
        assert_eq!(
            resolve(None, Some("/run/user/1001")),
            "/run/user/1001/kairos-quotes.sock"
        );
    }

    #[test]
    fn tmp_fallback_when_nothing_set() {
        assert_eq!(resolve(None, None), "/tmp/kairos-quotes.sock");
    }

    #[test]
    fn empty_values_are_ignored() {
        assert_eq!(resolve(Some(""), Some("")), "/tmp/kairos-quotes.sock");
        assert_eq!(
            resolve(Some(""), Some("/run/user/1001")),
            "/run/user/1001/kairos-quotes.sock"
        );
    }
}
