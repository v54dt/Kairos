//! Shared env-reading helpers so every "non-empty env else default" and
//! "positive-numeric env else default" read across core uses ONE spelling.

use std::str::FromStr;

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
