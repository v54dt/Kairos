//! The SIM namespace: an Aeron dir and quote/order sockets that are always
//! distinct from the live pipeline's. Every value is env-overridable but defaults
//! to a `-sim`-suffixed name so a sim can never share the live dir/sockets by
//! accident.

use crate::replay::marker::current_username;
use crate::uds::path::runtime_dir;

/// Default sim Aeron dir when `$KAIROS_SIM_AERON_DIR` is unset.
pub fn default_aeron_dir() -> String {
    format!("/dev/shm/aeron-{}-sim", current_username())
}

/// The resolved sim namespace. Distinct by construction from the live dir/sockets;
/// the isolation guard still verifies it against the live values before any spawn.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SimPaths {
    pub aeron_dir: String,
    pub quote_sock: String,
    pub order_sock: String,
}

impl SimPaths {
    /// Pure resolver: each field falls back to the given default when its override
    /// is `None`/empty. Kept parameterized so tests never touch the process env.
    pub fn resolve_with(
        aeron_override: Option<&str>,
        quote_override: Option<&str>,
        order_override: Option<&str>,
        runtime: Option<&str>,
    ) -> anyhow::Result<Self> {
        let aeron_dir = non_empty(aeron_override).unwrap_or_else(default_aeron_dir);
        let quote_sock = match non_empty(quote_override) {
            Some(p) => p,
            None => sock_in_runtime(runtime, "kairos-sim-quotes.sock", "KAIROS_SIM_QUOTE_SOCK")?,
        };
        let order_sock = match non_empty(order_override) {
            Some(p) => p,
            None => sock_in_runtime(runtime, "kairos-sim-orders.sock", "KAIROS_SIM_ORDER_SOCK")?,
        };
        Ok(Self {
            aeron_dir,
            quote_sock,
            order_sock,
        })
    }

    /// Resolve from the real environment: `$KAIROS_SIM_AERON_DIR`,
    /// `$KAIROS_SIM_QUOTE_SOCK`, `$KAIROS_SIM_ORDER_SOCK`, and `runtime_dir()`.
    pub fn resolve() -> anyhow::Result<Self> {
        Self::resolve_with(
            std::env::var("KAIROS_SIM_AERON_DIR").ok().as_deref(),
            std::env::var("KAIROS_SIM_QUOTE_SOCK").ok().as_deref(),
            std::env::var("KAIROS_SIM_ORDER_SOCK").ok().as_deref(),
            runtime_dir().as_deref(),
        )
    }
}

fn non_empty(v: Option<&str>) -> Option<String> {
    v.filter(|s| !s.is_empty()).map(str::to_owned)
}

fn sock_in_runtime(runtime: Option<&str>, base: &str, env: &str) -> anyhow::Result<String> {
    match runtime.filter(|d| !d.is_empty()) {
        Some(dir) => Ok(format!("{dir}/{base}")),
        None => anyhow::bail!("no runtime dir for the sim socket; set ${env} or $XDG_RUNTIME_DIR"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_are_sim_namespaced() {
        let p = SimPaths::resolve_with(None, None, None, Some("/run/user/1001")).unwrap();
        assert!(p.aeron_dir.ends_with("-sim"), "{}", p.aeron_dir);
        assert!(p.aeron_dir.starts_with("/dev/shm/aeron-"));
        assert_eq!(p.quote_sock, "/run/user/1001/kairos-sim-quotes.sock");
        assert_eq!(p.order_sock, "/run/user/1001/kairos-sim-orders.sock");
    }

    #[test]
    fn overrides_win() {
        let p = SimPaths::resolve_with(
            Some("/dev/shm/aeron-x-sim"),
            Some("/run/user/1001/q.sock"),
            Some("/run/user/1001/o.sock"),
            Some("/run/user/1001"),
        )
        .unwrap();
        assert_eq!(p.aeron_dir, "/dev/shm/aeron-x-sim");
        assert_eq!(p.quote_sock, "/run/user/1001/q.sock");
        assert_eq!(p.order_sock, "/run/user/1001/o.sock");
    }

    #[test]
    fn empty_override_falls_back_to_default() {
        let p =
            SimPaths::resolve_with(Some(""), Some(""), Some(""), Some("/run/user/1001")).unwrap();
        assert!(p.aeron_dir.ends_with("-sim"));
        assert_eq!(p.quote_sock, "/run/user/1001/kairos-sim-quotes.sock");
    }

    #[test]
    fn no_runtime_dir_errors_only_for_defaulted_sockets() {
        // No runtime dir but sockets given explicitly -> ok.
        assert!(SimPaths::resolve_with(None, Some("/a.sock"), Some("/b.sock"), None).is_ok());
        // No runtime dir and a socket must be defaulted -> error.
        assert!(SimPaths::resolve_with(None, None, Some("/b.sock"), None).is_err());
    }
}
