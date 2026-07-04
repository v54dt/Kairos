use anyhow::{Context, Result};
use tokio::process::Command;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UnitStatus {
    pub unit: String,
    pub load: String,
    pub active: String,
    pub sub: String,
    pub description: String,
}

/// Parse `systemctl --user list-units ... --plain` table rows. Rows are the
/// lines whose first column is a unit name (contains a '.'); the header, the
/// blank separator, the legend and the footer are all skipped.
pub fn parse_list_units(text: &str) -> Vec<UnitStatus> {
    let mut out = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() {
            break;
        }
        let mut cols = line.split_whitespace();
        let unit = match cols.next() {
            Some(u) if u.contains('.') => u,
            _ => continue,
        };
        let (Some(load), Some(active), Some(sub)) = (cols.next(), cols.next(), cols.next()) else {
            continue;
        };
        let description = cols.collect::<Vec<_>>().join(" ");
        out.push(UnitStatus {
            unit: unit.to_string(),
            load: load.to_string(),
            active: active.to_string(),
            sub: sub.to_string(),
            description,
        });
    }
    out
}

pub async fn list_units() -> Result<Vec<UnitStatus>> {
    let output = Command::new("systemctl")
        .args([
            "--user",
            "list-units",
            "kairos-*",
            "--all",
            "--no-pager",
            "--plain",
        ])
        .output()
        .await
        .context("spawn systemctl")?;
    if !output.status.success() {
        anyhow::bail!("systemctl exited with {}", output.status);
    }
    Ok(parse_list_units(&String::from_utf8_lossy(&output.stdout)))
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = "\
UNIT                       LOAD   ACTIVE   SUB     DESCRIPTION
kairos-core.service        loaded inactive dead    Kairos core (Aeron 1001 -> UDS quote server)
kairos-driver.service      loaded inactive dead    Kairos Aeron media driver
kairos-orderhub.service    loaded failed   failed  Kairos order hub (concords orders over UDS)
kairos-record-ship.service loaded inactive dead    Kairos KQR ship (compress + verify + backup + retention)
kairos-recordd.service     loaded inactive dead    Kairos KQR recorder (Aeron 1001+1002 -> dated KQR files)
kairos-sidecar.service     loaded inactive dead    Kairos concords sidecar (broker -> Aeron 1001)
kairos-record-ship.timer   loaded active   waiting Kairos KQR ship — weekday afternoon (system TZ is Asia/Taipei)

Legend: LOAD   → Reflects whether the unit definition was properly loaded.
        ACTIVE → The high-level unit activation state, i.e. generalization of SUB.
        SUB    → The low-level unit activation state, values depend on unit type.

7 loaded units listed.
To show all installed unit files use 'systemctl list-unit-files'.
";

    #[test]
    fn parses_rows_and_stops_at_blank() {
        let units = parse_list_units(SAMPLE);
        assert_eq!(units.len(), 7);
        assert_eq!(units[0].unit, "kairos-core.service");
        assert_eq!(units[0].load, "loaded");
        assert_eq!(units[0].active, "inactive");
        assert_eq!(units[0].sub, "dead");
        assert_eq!(
            units[0].description,
            "Kairos core (Aeron 1001 -> UDS quote server)"
        );
    }

    #[test]
    fn captures_failed_and_active_states() {
        let units = parse_list_units(SAMPLE);
        let hub = units
            .iter()
            .find(|u| u.unit == "kairos-orderhub.service")
            .unwrap();
        assert_eq!(hub.active, "failed");
        assert_eq!(hub.sub, "failed");
        let timer = units
            .iter()
            .find(|u| u.unit == "kairos-record-ship.timer")
            .unwrap();
        assert_eq!(timer.active, "active");
        assert_eq!(timer.sub, "waiting");
    }

    #[test]
    fn empty_input_yields_no_rows() {
        assert!(parse_list_units("").is_empty());
        assert!(parse_list_units("0 loaded units listed.\n").is_empty());
    }
}
