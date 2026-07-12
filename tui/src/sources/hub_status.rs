use std::path::{Path, PathBuf};
use std::time::Duration;

use super::json::{self, boolean as json_bool, int as json_int, string as json_str};

/// One scenario client the order hub is serving, as written to its status file.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ClientStatus {
    pub prefix: String,
    pub pid: i64,
    pub open: i64,
    pub submitted: i64,
    pub filled: i64,
    pub cancelled: i64,
    pub last_activity_s: i64,
}

/// Whole-hub snapshot parsed from `kairos-hub-status.json`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct HubStatus {
    pub start_epoch_s: i64,
    pub written_epoch_s: i64,
    pub client_count: i64,
    pub clients: Vec<ClientStatus>,
    pub account_open_notional_cents: i64,
    pub account_day_realized_cents: i64,
    pub max_account_notional_cents: i64,
    pub halted: bool,
}

/// A parsed snapshot plus how stale the file is (from its mtime).
#[derive(Clone, Debug)]
pub struct HubReport {
    pub status: HubStatus,
    pub age: Duration,
}

impl HubReport {
    pub fn is_stale(&self) -> bool {
        self.age > STALE_AFTER
    }
}

const STALE_AFTER: Duration = Duration::from_secs(10);

/// Hub status file path, mirroring the C++ `HubStatusPath()` resolution:
/// `$KAIROS_HUB_STATUS`, else `$XDG_RUNTIME_DIR`, else `/run/user/<uid>`.
pub fn hub_status_path() -> Option<PathBuf> {
    super::runtime_path::path("KAIROS_HUB_STATUS", "kairos-hub-status.json")
}

fn parse_client(obj: &str) -> ClientStatus {
    ClientStatus {
        prefix: json_str(obj, "prefix").unwrap_or_default(),
        pid: json_int(obj, "pid").unwrap_or(0),
        open: json_int(obj, "open").unwrap_or(0),
        submitted: json_int(obj, "submitted").unwrap_or(0),
        filled: json_int(obj, "filled").unwrap_or(0),
        cancelled: json_int(obj, "cancelled").unwrap_or(0),
        last_activity_s: json_int(obj, "last_activity_s").unwrap_or(0),
    }
}

/// Parse the hub status JSON. Missing scalar fields default to 0; a payload that
/// is not a JSON object (garbage/empty) is an error rather than a panic.
pub fn parse_hub_status(text: &str) -> Result<HubStatus, String> {
    let t = text.trim();
    if !t.starts_with('{') {
        return Err("not a JSON object".to_string());
    }
    Ok(HubStatus {
        start_epoch_s: json_int(t, "start_epoch_s").unwrap_or(0),
        written_epoch_s: json_int(t, "written_epoch_s").unwrap_or(0),
        client_count: json_int(t, "client_count").unwrap_or(0),
        clients: json::objects(t, "clients")
            .iter()
            .map(|o| parse_client(o))
            .collect(),
        account_open_notional_cents: json_int(t, "account_open_notional_cents").unwrap_or(0),
        account_day_realized_cents: json_int(t, "account_day_realized_cents").unwrap_or(0),
        max_account_notional_cents: json_int(t, "max_account_notional_cents").unwrap_or(0),
        halted: json_bool(t, "halted").unwrap_or(false),
    })
}

/// Read + parse the hub status file, age-stamped from its mtime. `None` when the
/// file is absent or unparseable (the caller renders "hub offline").
pub fn read_hub_status(path: &Path) -> Option<HubReport> {
    let meta = std::fs::metadata(path).ok()?;
    let age = meta
        .modified()
        .ok()
        .and_then(|m| m.elapsed().ok())
        .unwrap_or_default();
    let text = std::fs::read_to_string(path).ok()?;
    let status = parse_hub_status(&text).ok()?;
    Some(HubReport { status, age })
}

#[cfg(test)]
mod tests {
    use super::*;

    const GOOD: &str = "{\"start_epoch_s\":1000,\"written_epoch_s\":1042,\"client_count\":2,\
        \"clients\":[{\"prefix\":\"k100\",\"pid\":100,\"open\":2,\"submitted\":5,\"filled\":3,\
        \"cancelled\":1,\"last_activity_s\":1040},{\"prefix\":\"k200\",\"pid\":200,\"open\":0,\
        \"submitted\":1,\"filled\":1,\"cancelled\":0,\"last_activity_s\":1041}],\
        \"account_open_notional_cents\":1234500,\"account_day_realized_cents\":-5000,\
        \"max_account_notional_cents\":50000000,\"halted\":true}\n";

    #[test]
    fn parses_good_snapshot() {
        let s = parse_hub_status(GOOD).unwrap();
        assert_eq!(s.start_epoch_s, 1000);
        assert_eq!(s.written_epoch_s, 1042);
        assert_eq!(s.client_count, 2);
        assert_eq!(s.clients.len(), 2);
        assert_eq!(s.clients[0].prefix, "k100");
        assert_eq!(s.clients[0].pid, 100);
        assert_eq!(s.clients[0].open, 2);
        assert_eq!(s.clients[0].filled, 3);
        assert_eq!(s.clients[1].prefix, "k200");
        assert_eq!(s.clients[1].cancelled, 0);
        assert_eq!(s.account_open_notional_cents, 1234500);
        assert_eq!(s.account_day_realized_cents, -5000);
        assert_eq!(s.max_account_notional_cents, 50000000);
        assert!(s.halted);
    }

    #[test]
    fn risk_fields_default_when_absent() {
        // A payload lacking the risk fields must still parse (Scenarios panel
        // regression guard): defaults are 0 and halted false.
        let s = parse_hub_status("{\"client_count\":0,\"clients\":[]}").unwrap();
        assert_eq!(s.account_open_notional_cents, 0);
        assert_eq!(s.account_day_realized_cents, 0);
        assert_eq!(s.max_account_notional_cents, 0);
        assert!(!s.halted);
    }

    #[test]
    fn halted_true_and_false_parse() {
        let t = parse_hub_status("{\"clients\":[],\"halted\":true}").unwrap();
        assert!(t.halted);
        let f = parse_hub_status("{\"clients\":[],\"halted\":false}").unwrap();
        assert!(!f.halted);
    }

    #[test]
    fn garbage_is_err_not_panic() {
        assert!(parse_hub_status("").is_err());
        assert!(parse_hub_status("not json").is_err());
        assert!(parse_hub_status("[1,2,3]").is_err());
    }

    #[test]
    fn missing_fields_default_to_zero() {
        let s = parse_hub_status("{\"client_count\":0,\"clients\":[]}").unwrap();
        assert_eq!(s.start_epoch_s, 0);
        assert_eq!(s.written_epoch_s, 0);
        assert_eq!(s.client_count, 0);
        assert!(s.clients.is_empty());
    }

    #[test]
    fn client_missing_fields_default() {
        let s = parse_hub_status("{\"clients\":[{\"prefix\":\"k7\"}]}").unwrap();
        assert_eq!(s.clients.len(), 1);
        assert_eq!(s.clients[0].prefix, "k7");
        assert_eq!(s.clients[0].pid, 0);
        assert_eq!(s.clients[0].open, 0);
    }

    // CROSS-SIDE: exact bytes emitted by the C++ SerializeHubStatus (copied
    // verbatim from a real emit run of the upgraded serializer) for a client
    // prefix carrying newline, tab, quote, backslash, and CJK. The upgraded TUI
    // unescaper must recover the original prefix, and the object splitter must not
    // be fooled by the escaped quote inside the string.
    const CPP_HOSTILE_HUB: &str = "{\"start_epoch_s\":1000,\"written_epoch_s\":1042,\
        \"client_count\":1,\"clients\":[{\"prefix\":\"k7\\nx\\ty\\\"z\\\\w 台積電\",\"pid\":7,\
        \"open\":1,\"submitted\":2,\"filled\":1,\"cancelled\":0,\"last_activity_s\":1040}],\
        \"account_open_notional_cents\":0,\"account_day_realized_cents\":0,\
        \"max_account_notional_cents\":0,\"halted\":false}\n";

    #[test]
    fn cross_side_hostile_prefix_round_trips() {
        let s = parse_hub_status(CPP_HOSTILE_HUB).unwrap();
        assert_eq!(s.client_count, 1);
        assert_eq!(s.clients.len(), 1);
        assert_eq!(s.clients[0].prefix, "k7\nx\ty\"z\\w 台積電");
        assert_eq!(s.clients[0].pid, 7);
        assert_eq!(s.clients[0].filled, 1);
    }

    #[test]
    fn stale_threshold() {
        let r = HubReport {
            status: HubStatus::default(),
            age: Duration::from_secs(5),
        };
        assert!(!r.is_stale());
        let r = HubReport {
            status: HubStatus::default(),
            age: Duration::from_secs(11),
        };
        assert!(r.is_stale());
    }

    use crate::sources::runtime_path::resolve;

    const BASE: &str = "kairos-hub-status.json";

    #[test]
    fn resolver_matches_socket_convention() {
        assert_eq!(
            resolve(Some("/run/hub.json"), Some("/run/user/1001"), None, BASE),
            Some("/run/hub.json".to_string())
        );
        assert_eq!(
            resolve(None, Some("/run/user/1001"), Some("/run/user/1001"), BASE),
            Some("/run/user/1001/kairos-hub-status.json".to_string())
        );
        assert_eq!(
            resolve(None, None, Some("/run/user/1001"), BASE),
            Some("/run/user/1001/kairos-hub-status.json".to_string())
        );
        assert_eq!(resolve(None, None, None, BASE), None);
        assert_eq!(resolve(Some(""), Some(""), Some(""), BASE), None);
    }

    // Shared cross-language golden: rows for this module's base must resolve the
    // same here as in core resolve() and exec ResolveSock.
    #[test]
    fn golden_runtime_paths_hub_status() {
        let fixture = include_str!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../schema/testdata/runtime_paths.txt"
        ));
        fn token(t: &str) -> Option<&str> {
            match t {
                "UNSET" => None,
                "EMPTY" => Some(""),
                v => Some(v),
            }
        }
        let mut rows = 0;
        for line in fixture.lines() {
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let f: Vec<&str> = line.split('|').collect();
            assert_eq!(f.len(), 5, "bad row: {line}");
            if f[3] != "kairos-hub-status.json" {
                continue;
            }
            let ru = match f[2] {
                "yes" => Some("/run/user/1000"),
                "no" => None,
                other => panic!("bad run_user: {other}"),
            };
            let got = resolve(token(f[0]), token(f[1]), ru, BASE);
            let want = (f[4] != "FATAL").then(|| f[4].to_string());
            assert_eq!(got, want, "row: {line}");
            rows += 1;
        }
        assert!(rows >= 8, "no rows for kairos-hub-status.json: {rows}");
    }
}
