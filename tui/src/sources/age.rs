use std::time::Duration;

pub fn format_age(d: Duration) -> String {
    let secs = d.as_secs();
    if secs < 60 {
        format!("{secs}s")
    } else if secs < 3600 {
        format!("{}m{:02}s", secs / 60, secs % 60)
    } else if secs < 86_400 {
        format!("{}h{:02}m", secs / 3600, (secs % 3600) / 60)
    } else {
        format!("{}d{:02}h", secs / 86_400, (secs % 86_400) / 3600)
    }
}

/// Compact single-unit age for the scenarios "last fill" column: whole seconds
/// under a minute, whole minutes under an hour, else whole hours.
pub fn format_fill_age(d: Duration) -> String {
    let secs = d.as_secs();
    if secs < 60 {
        format!("{secs}s")
    } else if secs < 3600 {
        format!("{}m", secs / 60)
    } else {
        format!("{}h", secs / 3600)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fill_age_single_unit() {
        assert_eq!(format_fill_age(Duration::from_secs(0)), "0s");
        assert_eq!(format_fill_age(Duration::from_secs(59)), "59s");
        assert_eq!(format_fill_age(Duration::from_secs(90)), "1m");
        assert_eq!(format_fill_age(Duration::from_secs(3599)), "59m");
        assert_eq!(format_fill_age(Duration::from_secs(3600)), "1h");
        assert_eq!(format_fill_age(Duration::from_secs(7200)), "2h");
    }

    #[test]
    fn seconds() {
        assert_eq!(format_age(Duration::from_secs(0)), "0s");
        assert_eq!(format_age(Duration::from_secs(59)), "59s");
    }

    #[test]
    fn minutes() {
        assert_eq!(format_age(Duration::from_secs(60)), "1m00s");
        assert_eq!(format_age(Duration::from_secs(125)), "2m05s");
        assert_eq!(format_age(Duration::from_secs(3599)), "59m59s");
    }

    #[test]
    fn hours() {
        assert_eq!(format_age(Duration::from_secs(3600)), "1h00m");
        assert_eq!(format_age(Duration::from_secs(3661)), "1h01m");
    }

    #[test]
    fn days() {
        assert_eq!(format_age(Duration::from_secs(86_400)), "1d00h");
        assert_eq!(format_age(Duration::from_secs(90_000)), "1d01h");
    }

    #[test]
    fn sub_second_truncates() {
        assert_eq!(format_age(Duration::from_millis(1500)), "1s");
    }
}
