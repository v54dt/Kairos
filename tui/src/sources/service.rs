use tokio::process::Command;

/// A systemd management verb. Typed so a restart can never masquerade as a
/// reset-failed through the safe path.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Verb {
    Restart,
    Start,
    Stop,
    ResetFailed,
}

impl Verb {
    pub fn as_str(self) -> &'static str {
        match self {
            Verb::Restart => "restart",
            Verb::Start => "start",
            Verb::Stop => "stop",
            Verb::ResetFailed => "reset-failed",
        }
    }
}

/// Confirmation strength required before a management action fires.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Strength {
    Dangerous,
    Safe,
}

/// The unit name without its final `.service`/`.timer`/`.target` extension.
/// A name with no `.` is its own stem (kept whole for the fail-safe default).
fn unit_stem(unit: &str) -> &str {
    match unit.rsplit_once('.') {
        Some((stem, _)) => stem,
        None => unit,
    }
}

/// Explicit SAFE stems: research crons + harmless ops. Everything else is
/// DANGEROUS by fail-safe default.
const SAFE_STEMS: [&str; 5] = [
    "kairos-lab-ticks",
    "kairos-lab-ssf",
    "kairos-lab-chips",
    "kairos-lab-blacklist",
    "kairos-bsr",
];

/// Classify a management action. `reset-failed` only clears a status marker and
/// never touches a process, so it is SAFE for any unit. Otherwise the unit stem
/// must be in the explicit SAFE allowlist; an unknown/unclassified name defaults
/// to DANGEROUS (typed confirm), never the easy path.
pub fn classify(verb: Verb, unit: &str) -> Strength {
    if verb == Verb::ResetFailed {
        return Strength::Safe;
    }
    if SAFE_STEMS.contains(&unit_stem(unit)) {
        Strength::Safe
    } else {
        Strength::Dangerous
    }
}

/// The exact argv for a management action: `systemctl --user VERB UNIT`.
pub fn build_argv(verb: Verb, unit: &str) -> Vec<String> {
    vec![
        "systemctl".to_string(),
        "--user".to_string(),
        verb.as_str().to_string(),
        unit.to_string(),
    ]
}

/// Run `systemctl --user VERB UNIT`, capturing stdout+stderr+exit. Always
/// returns a human line (success or the error text); never propagates, so a
/// failure is surfaced inline rather than swallowed.
pub async fn run_action(verb: Verb, unit: &str) -> String {
    let argv = build_argv(verb, unit);
    let output = Command::new(&argv[0]).args(&argv[1..]).output().await;
    match output {
        Ok(o) if o.status.success() => format!("{} {} ok", verb.as_str(), unit),
        Ok(o) => {
            let mut msg = String::from_utf8_lossy(&o.stderr).trim().to_string();
            if msg.is_empty() {
                msg = String::from_utf8_lossy(&o.stdout).trim().to_string();
            }
            if msg.is_empty() {
                msg = format!("exited with {}", o.status);
            }
            format!("{} {} failed: {}", verb.as_str(), unit, msg)
        }
        Err(e) => format!("{} {} failed: {}", verb.as_str(), unit, e),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const DANGEROUS_STEMS: [&str; 6] = [
        "kairos-driver",
        "kairos-core",
        "kairos-sidecar",
        "kairos-orderhub",
        "kairos-recordd",
        "kairos-record-ship",
    ];

    #[test]
    fn verb_strings_match_systemctl() {
        assert_eq!(Verb::Restart.as_str(), "restart");
        assert_eq!(Verb::Start.as_str(), "start");
        assert_eq!(Verb::Stop.as_str(), "stop");
        assert_eq!(Verb::ResetFailed.as_str(), "reset-failed");
    }

    #[test]
    fn classify_dangerous() {
        for stem in DANGEROUS_STEMS {
            for ext in [".service", ".timer"] {
                let unit = format!("{stem}{ext}");
                for verb in [Verb::Restart, Verb::Start, Verb::Stop] {
                    assert_eq!(
                        classify(verb, &unit),
                        Strength::Dangerous,
                        "{verb:?} {unit}"
                    );
                }
            }
        }
        for verb in [Verb::Restart, Verb::Start, Verb::Stop] {
            assert_eq!(classify(verb, "kairos.target"), Strength::Dangerous);
        }
    }

    #[test]
    fn classify_safe() {
        for stem in SAFE_STEMS {
            for ext in [".service", ".timer"] {
                let unit = format!("{stem}{ext}");
                for verb in [Verb::Restart, Verb::Start, Verb::Stop] {
                    assert_eq!(classify(verb, &unit), Strength::Safe, "{verb:?} {unit}");
                }
            }
        }
    }

    #[test]
    fn reset_failed_always_safe() {
        assert_eq!(
            classify(Verb::ResetFailed, "kairos-driver.service"),
            Strength::Safe
        );
        assert_eq!(classify(Verb::ResetFailed, "kairos.target"), Strength::Safe);
        assert_eq!(
            classify(Verb::ResetFailed, "totally-unknown.service"),
            Strength::Safe
        );
    }

    #[test]
    fn unknown_is_dangerous() {
        for unit in [
            "kairos-foo.service",
            "random.service",
            "kairos-driverx.service",
            "kairos-lab-ticksx.service",
            "kairos",
        ] {
            assert_eq!(
                classify(Verb::Start, unit),
                Strength::Dangerous,
                "{unit} must default dangerous"
            );
        }
    }

    #[test]
    fn build_argv_is_exact() {
        assert_eq!(
            build_argv(Verb::Restart, "kairos-driver.service"),
            vec!["systemctl", "--user", "restart", "kairos-driver.service"]
        );
        assert_eq!(
            build_argv(Verb::ResetFailed, "kairos-orderhub.service"),
            vec![
                "systemctl",
                "--user",
                "reset-failed",
                "kairos-orderhub.service"
            ]
        );
        assert_eq!(
            build_argv(Verb::Stop, "kairos.target"),
            vec!["systemctl", "--user", "stop", "kairos.target"]
        );
    }
}
