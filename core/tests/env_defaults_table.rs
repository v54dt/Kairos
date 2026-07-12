//! Var-by-var proof that every server-relevant env knob keeps its EXACT old
//! default and validity rule after the RF3b consolidation: unset -> default,
//! garbage -> default (or a fatal error for the two validated vars), set -> value.
//! One test fn so the process-global env is mutated serially.

use kairos_core::config::Config;
use kairos_core::failover::{DEFAULT_RECOVER_HOLD_MS, DEFAULT_STALE_MS};
use kairos_core::watchdog::{DEFAULT_DRIVER_TIMEOUT_MS, DEFAULT_MAX_POLL_ERRORS};

const SERVER_VARS: &[&str] = &[
    "KAIROS_STREAMS",
    "KAIROS_SOURCE_PRIORITY",
    "KAIROS_FAILOVER_STALE_MS",
    "KAIROS_FAILOVER_RECOVER_HOLD_MS",
    "KAIROS_AERON_MAX_POLL_ERRORS",
    "KAIROS_DRIVER_TIMEOUT_MS",
    "KAIROS_AERON_DIR",
    "KAIROS_QUOTE_SOCK",
];

fn set(key: &str, val: &str) {
    unsafe { std::env::set_var(key, val) };
}

fn unset(key: &str) {
    unsafe { std::env::remove_var(key) };
}

/// A clean baseline: every server var unset, then a fixed quote sock so
/// `Config::from_env` never exits on a missing runtime dir.
fn baseline() {
    for v in SERVER_VARS {
        unset(v);
    }
    set("KAIROS_QUOTE_SOCK", "/run/user/1000/kairos-quotes.sock");
}

fn build() -> Config {
    match Config::from_env() {
        Ok(cfg) => cfg,
        Err(m) => panic!("valid config expected: {m}"),
    }
}

fn build_err() -> String {
    match Config::from_env() {
        Ok(_) => panic!("expected a config error"),
        Err(m) => m,
    }
}

#[test]
fn every_server_var_keeps_its_default_and_rule() {
    // KAIROS_FAILOVER_STALE_MS: unset/garbage/zero -> default; positive -> value.
    baseline();
    assert_eq!(build().failover_stale_ms, DEFAULT_STALE_MS);
    assert_eq!(DEFAULT_STALE_MS, 2_000, "must match docs/RUNBOOK.md");
    set("KAIROS_FAILOVER_STALE_MS", "garbage");
    assert_eq!(build().failover_stale_ms, DEFAULT_STALE_MS);
    set("KAIROS_FAILOVER_STALE_MS", "0");
    assert_eq!(build().failover_stale_ms, DEFAULT_STALE_MS);
    set("KAIROS_FAILOVER_STALE_MS", "3000");
    assert_eq!(build().failover_stale_ms, 3_000);

    // KAIROS_FAILOVER_RECOVER_HOLD_MS: same rule.
    baseline();
    assert_eq!(build().failover_recover_hold_ms, DEFAULT_RECOVER_HOLD_MS);
    assert_eq!(DEFAULT_RECOVER_HOLD_MS, 5_000, "must match docs/RUNBOOK.md");
    set("KAIROS_FAILOVER_RECOVER_HOLD_MS", "nope");
    assert_eq!(build().failover_recover_hold_ms, DEFAULT_RECOVER_HOLD_MS);
    set("KAIROS_FAILOVER_RECOVER_HOLD_MS", "0");
    assert_eq!(build().failover_recover_hold_ms, DEFAULT_RECOVER_HOLD_MS);
    set("KAIROS_FAILOVER_RECOVER_HOLD_MS", "8000");
    assert_eq!(build().failover_recover_hold_ms, 8_000);

    // KAIROS_AERON_MAX_POLL_ERRORS: unset/garbage/zero -> default; positive -> value.
    baseline();
    assert_eq!(build().max_poll_errors, DEFAULT_MAX_POLL_ERRORS);
    assert_eq!(DEFAULT_MAX_POLL_ERRORS, 64, "must match docs/RUNBOOK.md");
    set("KAIROS_AERON_MAX_POLL_ERRORS", "x");
    assert_eq!(build().max_poll_errors, DEFAULT_MAX_POLL_ERRORS);
    set("KAIROS_AERON_MAX_POLL_ERRORS", "0");
    assert_eq!(build().max_poll_errors, DEFAULT_MAX_POLL_ERRORS);
    set("KAIROS_AERON_MAX_POLL_ERRORS", "8");
    assert_eq!(build().max_poll_errors, 8);

    // KAIROS_DRIVER_TIMEOUT_MS: unset/garbage/non-positive -> default; positive -> value.
    baseline();
    assert_eq!(build().driver_timeout_ms, DEFAULT_DRIVER_TIMEOUT_MS);
    assert_eq!(
        DEFAULT_DRIVER_TIMEOUT_MS, 10_000,
        "must match docs/RUNBOOK.md"
    );
    set("KAIROS_DRIVER_TIMEOUT_MS", "x");
    assert_eq!(build().driver_timeout_ms, DEFAULT_DRIVER_TIMEOUT_MS);
    set("KAIROS_DRIVER_TIMEOUT_MS", "-5");
    assert_eq!(build().driver_timeout_ms, DEFAULT_DRIVER_TIMEOUT_MS);
    set("KAIROS_DRIVER_TIMEOUT_MS", "20000");
    assert_eq!(build().driver_timeout_ms, 20_000);

    // KAIROS_STREAMS: unset -> default table (quotes 1001:0 + control 1002);
    // garbage -> fatal error; valid -> parsed.
    baseline();
    let cfg = build();
    assert_eq!(cfg.streams.entries().len(), 2);
    assert_eq!(cfg.priority, vec![0]);
    set("KAIROS_STREAMS", "1001");
    let err = build_err();
    assert!(err.starts_with("invalid KAIROS_STREAMS:"), "{err}");
    set(
        "KAIROS_STREAMS",
        "1001:0:quotes,1003:1:quotes,1002:0:control",
    );
    let cfg = build();
    assert_eq!(cfg.streams.entries().len(), 3);
    assert_eq!(cfg.priority, vec![0, 1]);

    // KAIROS_SOURCE_PRIORITY: unset -> declared order; phantom -> fatal error;
    // valid reorder -> value. (Two-feed table still set from above.)
    set("KAIROS_SOURCE_PRIORITY", "9,9");
    let err = build_err();
    assert!(err.starts_with("invalid KAIROS_SOURCE_PRIORITY:"), "{err}");
    set("KAIROS_SOURCE_PRIORITY", "1,0");
    assert_eq!(build().priority, vec![1, 0]);

    // KAIROS_AERON_DIR: empty -> falls back to the native default; set -> that dir.
    baseline();
    let default_dir = build().aeron_dir;
    assert!(default_dir.is_some(), "native default is always Some");
    set("KAIROS_AERON_DIR", "");
    assert_eq!(build().aeron_dir, default_dir);
    set("KAIROS_AERON_DIR", "/dev/shm/aeron-rf3b-test");
    assert_eq!(
        build().aeron_dir.as_deref(),
        Some("/dev/shm/aeron-rf3b-test")
    );

    // KAIROS_QUOTE_SOCK: set -> that path.
    baseline();
    set("KAIROS_QUOTE_SOCK", "/run/user/1000/rf3b-quotes.sock");
    assert_eq!(build().quote_sock, "/run/user/1000/rf3b-quotes.sock");

    for v in SERVER_VARS {
        unset(v);
    }
}
