//! Shared env-reading helpers so every "non-empty env else default" and
//! "positive-numeric env else default" read across core uses ONE spelling, plus
//! the live server's single startup configuration build point.

use std::str::FromStr;

use crate::failover::{DEFAULT_RECOVER_HOLD_MS, DEFAULT_STALE_MS};
use crate::replay::marker::effective_stack_dir;
use crate::streams::{StreamRole, StreamTable};
use crate::uds::path::quote_socket_path;
use crate::watchdog::{driver_timeout_ms_from_env, max_poll_errors_from_env};

/// The value of `key` if set and non-empty, else `None`. A blank value is treated
/// as unset everywhere in the pipeline.
pub fn env_nonempty(key: &str) -> Option<String> {
    std::env::var(key).ok().filter(|v| !v.is_empty())
}

/// Parse `key` as `T`, falling back to `default` when unset, unparseable, or not
/// strictly positive. The positive-value filter matches the numeric knobs that all
/// reject a zero/negative value (poll-error budget, driver timeout, failover ms).
pub fn env_parsed<T>(key: &str, default: T) -> T
where
    T: FromStr + Default + PartialOrd,
{
    std::env::var(key)
        .ok()
        .and_then(|v| v.parse::<T>().ok())
        .filter(|v| *v > T::default())
        .unwrap_or(default)
}

/// Print a fatal message in the pipeline's standard form and exit non-zero. The
/// single funnel for the live server's config-validation exits.
pub fn fatal(msg: &str) -> ! {
    eprintln!("kairos-core: FATAL {msg}");
    std::process::exit(1);
}

/// The live server's (`kairos-core`) effective configuration, built ONCE at startup
/// from the environment so every component reads one coherent view instead of
/// re-resolving vars at scattered call sites.
pub struct Config {
    pub streams: StreamTable,
    pub priority: Vec<u16>,
    pub failover_stale_ms: u64,
    pub failover_recover_hold_ms: u64,
    pub max_poll_errors: u32,
    pub driver_timeout_ms: i64,
    pub aeron_dir: Option<String>,
    pub quote_sock: String,
}

impl Config {
    /// Gather every server-relevant var. Only the stream-table and source-priority
    /// reads can fail (invalid `KAIROS_STREAMS` / `KAIROS_SOURCE_PRIORITY`); the
    /// caller turns the error into a fatal exit before touching Aeron.
    pub fn from_env() -> Result<Self, String> {
        let streams =
            StreamTable::from_env().map_err(|e| format!("invalid KAIROS_STREAMS: {e:?}"))?;
        let priority = streams
            .source_priority(std::env::var("KAIROS_SOURCE_PRIORITY").ok().as_deref())
            .map_err(|e| format!("invalid KAIROS_SOURCE_PRIORITY: {e:?}"))?;
        Ok(Self {
            streams,
            priority,
            failover_stale_ms: env_parsed("KAIROS_FAILOVER_STALE_MS", DEFAULT_STALE_MS),
            failover_recover_hold_ms: env_parsed(
                "KAIROS_FAILOVER_RECOVER_HOLD_MS",
                DEFAULT_RECOVER_HOLD_MS,
            ),
            max_poll_errors: max_poll_errors_from_env(),
            driver_timeout_ms: driver_timeout_ms_from_env(),
            aeron_dir: effective_stack_dir(None),
            quote_sock: quote_socket_path(),
        })
    }

    /// One greppable English line summarizing the effective config, logged at startup.
    pub fn describe(&self) -> String {
        let streams: Vec<String> = self
            .streams
            .entries()
            .iter()
            .map(|e| {
                let role = match e.role {
                    StreamRole::Quotes => "quotes",
                    StreamRole::Control => "control",
                };
                format!("{}:{}:{}", e.stream_id, e.source, role)
            })
            .collect();
        let aeron_dir = self.aeron_dir.as_deref().unwrap_or("default");
        format!(
            "effective config streams=[{}] priority={:?} failover_stale_ms={} \
             failover_recover_hold_ms={} max_poll_errors={} driver_timeout_ms={} \
             aeron_dir={} quote_sock={}",
            streams.join(","),
            self.priority,
            self.failover_stale_ms,
            self.failover_recover_hold_ms,
            self.max_poll_errors,
            self.driver_timeout_ms,
            aeron_dir,
            self.quote_sock,
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn env_nonempty_unset_empty_set() {
        unsafe { std::env::remove_var("RF3B_ENV_NONEMPTY") };
        assert_eq!(env_nonempty("RF3B_ENV_NONEMPTY"), None);
        unsafe { std::env::set_var("RF3B_ENV_NONEMPTY", "") };
        assert_eq!(env_nonempty("RF3B_ENV_NONEMPTY"), None);
        unsafe { std::env::set_var("RF3B_ENV_NONEMPTY", "x") };
        assert_eq!(env_nonempty("RF3B_ENV_NONEMPTY").as_deref(), Some("x"));
        unsafe { std::env::remove_var("RF3B_ENV_NONEMPTY") };
    }

    #[test]
    fn env_parsed_rejects_unset_garbage_and_non_positive() {
        unsafe { std::env::remove_var("RF3B_ENV_PARSED") };
        assert_eq!(env_parsed::<u64>("RF3B_ENV_PARSED", 7), 7);
        unsafe { std::env::set_var("RF3B_ENV_PARSED", "nope") };
        assert_eq!(env_parsed::<u64>("RF3B_ENV_PARSED", 7), 7);
        unsafe { std::env::set_var("RF3B_ENV_PARSED", "0") };
        assert_eq!(env_parsed::<u64>("RF3B_ENV_PARSED", 7), 7);
        unsafe { std::env::set_var("RF3B_ENV_PARSED", "-3") };
        assert_eq!(env_parsed::<i64>("RF3B_ENV_PARSED", 7), 7);
        unsafe { std::env::set_var("RF3B_ENV_PARSED", "42") };
        assert_eq!(env_parsed::<u64>("RF3B_ENV_PARSED", 7), 42);
        unsafe { std::env::remove_var("RF3B_ENV_PARSED") };
    }
}
