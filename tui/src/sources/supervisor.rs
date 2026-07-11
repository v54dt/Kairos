//! Scenario supervisor control client: speaks the S1 `kairos_scenario_supervisord`
//! JSON-lines protocol over its UDS. The daemon is the single source of truth for
//! scenario-trader state; this source polls `{"cmd":"list"}` and exposes start/stop
//! senders. Every path degrades gracefully: a missing socket, a down daemon, or a
//! malformed reply yields a "not connected" state, never a panic or a blocked UI.

use std::path::{Path, PathBuf};
use std::time::Duration;

use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::UnixStream;

/// Bound every control IO so a wedged daemon can only stall this task, never the
/// render loop.
const IO_TIMEOUT: Duration = Duration::from_secs(2);

/// The launch mode a start requests. Only `Live` maps to real-money orders and is
/// only ever produced by the TUI's typed-stem confirm path.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    Paper,
    Live,
    Test,
}

impl Mode {
    /// The exact wire token, matching `ScenarioModeName` in the C++ proto.
    pub fn token(self) -> &'static str {
        match self {
            Mode::Paper => "paper",
            Mode::Live => "live",
            Mode::Test => "test",
        }
    }
}

/// Per-scenario lifecycle, mirroring the S1 `StateName()` strings. A running state
/// (starting/wait-open/in-window/fill-remainder) renders GREEN; everything else
/// (incl. stopping/closed-exited/crashed) renders RED. An unrecognized token is
/// surfaced verbatim rather than silently mislabeled as running.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ScenarioState {
    Stopped,
    Starting,
    WaitOpen,
    InWindow,
    FillRemainder,
    ClosedExited,
    Crashed,
    Stopping,
    Unknown(String),
}

impl ScenarioState {
    pub fn parse(s: &str) -> ScenarioState {
        match s {
            "stopped" => ScenarioState::Stopped,
            "starting" => ScenarioState::Starting,
            "wait-open" => ScenarioState::WaitOpen,
            "in-window" => ScenarioState::InWindow,
            "fill-remainder" => ScenarioState::FillRemainder,
            "closed-exited" => ScenarioState::ClosedExited,
            "crashed" => ScenarioState::Crashed,
            "stopping" => ScenarioState::Stopping,
            other => ScenarioState::Unknown(other.to_string()),
        }
    }

    pub fn name(&self) -> String {
        match self {
            ScenarioState::Stopped => "stopped".to_string(),
            ScenarioState::Starting => "starting".to_string(),
            ScenarioState::WaitOpen => "wait-open".to_string(),
            ScenarioState::InWindow => "in-window".to_string(),
            ScenarioState::FillRemainder => "fill-remainder".to_string(),
            ScenarioState::ClosedExited => "closed-exited".to_string(),
            ScenarioState::Crashed => "crashed".to_string(),
            ScenarioState::Stopping => "stopping".to_string(),
            ScenarioState::Unknown(s) => s.clone(),
        }
    }

    /// True only for the phases where the daemon owns a live, running child.
    pub fn is_running(&self) -> bool {
        matches!(
            self,
            ScenarioState::Starting
                | ScenarioState::WaitOpen
                | ScenarioState::InWindow
                | ScenarioState::FillRemainder
        )
    }
}

/// One scenario row from the supervisor snapshot.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SupervisorRow {
    pub name: String,
    pub state: ScenarioState,
    pub pid: i64,
    pub cum_fills: i64,
    pub cum_shares: i64,
    pub last_fill_ts: i64,
    pub last_exit_reason: String,
    pub live: bool,
}

/// The whole supervisor view the panel renders. `connected` is false whenever the
/// last poll could not reach or parse the daemon; `last_error` carries why. The
/// per-command ok/err belongs to start/stop replies (see [`send_command`]), not to
/// a list snapshot, so it is not held here.
#[derive(Clone, Debug, Default)]
pub struct SupervisorState {
    pub connected: bool,
    pub last_error: Option<String>,
    pub rows: Vec<SupervisorRow>,
}

fn resolve(explicit: Option<&str>, xdg: Option<&str>, run_user: Option<&str>) -> Option<String> {
    if let Some(p) = explicit
        && !p.is_empty()
    {
        return Some(p.to_string());
    }
    if let Some(dir) = xdg
        && !dir.is_empty()
    {
        return Some(format!("{dir}/kairos-scenario-ctl.sock"));
    }
    if let Some(dir) = run_user
        && !dir.is_empty()
    {
        return Some(format!("{dir}/kairos-scenario-ctl.sock"));
    }
    None
}

fn run_user_dir() -> Option<String> {
    // SAFETY: getuid() is infallible and has no preconditions.
    let dir = format!("/run/user/{}", unsafe { libc::getuid() });
    Path::new(&dir).is_dir().then_some(dir)
}

/// Control socket path, mirroring the C++ `ScenarioCtlSocketPath()` resolution:
/// `$KAIROS_SCENARIO_CTL_SOCK`, else `$XDG_RUNTIME_DIR`, else `/run/user/<uid>`.
pub fn supervisor_ctl_path() -> Option<PathBuf> {
    resolve(
        std::env::var("KAIROS_SCENARIO_CTL_SOCK").ok().as_deref(),
        std::env::var("XDG_RUNTIME_DIR").ok().as_deref(),
        run_user_dir().as_deref(),
    )
    .map(PathBuf::from)
}

/// Escape a string for a JSON literal, matching the exec `JsonEscape` superset
/// (`\" \\ \b \f \n \r \t` + `\u00xx` controls) so a scenario name with any
/// control byte round-trips through the C++ ctl-server parser exactly.
fn json_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 8);
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\u{08}' => out.push_str("\\b"),
            '\u{0c}' => out.push_str("\\f"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out
}

/// `{"cmd":"list"}\n` — the status poll.
pub fn list_request() -> String {
    "{\"cmd\":\"list\"}\n".to_string()
}

/// `{"cmd":"start","name":"<name>","mode":"<token>"}\n`. The `live` token is only
/// ever passed here from a `Mode::Live`, which the confirm layer produces only via
/// the typed-stem path.
pub fn start_request(name: &str, mode: Mode) -> String {
    format!(
        "{{\"cmd\":\"start\",\"name\":\"{}\",\"mode\":\"{}\"}}\n",
        json_escape(name),
        mode.token()
    )
}

/// `{"cmd":"stop","name":"<name>"}\n`.
pub fn stop_request(name: &str) -> String {
    format!("{{\"cmd\":\"stop\",\"name\":\"{}\"}}\n", json_escape(name))
}

fn json_int(s: &str, key: &str) -> Option<i64> {
    let needle = format!("\"{key}\":");
    let start = s.find(&needle)? + needle.len();
    let rest = &s[start..];
    let end = rest
        .find(|c: char| c != '-' && !c.is_ascii_digit())
        .unwrap_or(rest.len());
    rest[..end].parse().ok()
}

fn json_bool(s: &str, key: &str) -> Option<bool> {
    let needle = format!("\"{key}\":");
    let start = s.find(&needle)? + needle.len();
    let rest = s[start..].trim_start();
    if rest.starts_with("true") {
        Some(true)
    } else if rest.starts_with("false") {
        Some(false)
    } else {
        None
    }
}

// Read the JSON string value of `"key":"..."`, decoding every escape the exec
// emitters produce (\" \\ \/ \b \f \n \r \t \uXXXX) and stopping at the first
// UNescaped quote. Unknown/short escapes are kept verbatim (fail-soft: at worst
// a garbled display char, never a panic) so a new-server payload degrades safely.
fn json_str(s: &str, key: &str) -> Option<String> {
    let needle = format!("\"{key}\":\"");
    let start = s.find(&needle)? + needle.len();
    let mut out = String::new();
    let mut chars = s[start..].chars();
    while let Some(c) = chars.next() {
        match c {
            '"' => return Some(out),
            '\\' => match chars.next() {
                Some('"') => out.push('"'),
                Some('\\') => out.push('\\'),
                Some('/') => out.push('/'),
                Some('b') => out.push('\u{08}'),
                Some('f') => out.push('\u{0c}'),
                Some('n') => out.push('\n'),
                Some('r') => out.push('\r'),
                Some('t') => out.push('\t'),
                Some('u') => {
                    let hex: String = (&mut chars).take(4).collect();
                    let cp = (hex.len() == 4)
                        .then(|| u32::from_str_radix(&hex, 16).ok())
                        .flatten();
                    out.push(cp.and_then(char::from_u32).unwrap_or('\u{fffd}'));
                }
                Some(other) => {
                    out.push('\\');
                    out.push(other);
                }
                None => break,
            },
            c => out.push(c),
        }
    }
    Some(out)
}

/// Split the `"scenarios":[ {..}, {..} ]` array into its top-level `{..}` objects.
fn scenario_objects(s: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let arr_start = match s.find("\"scenarios\":[") {
        Some(i) => i + "\"scenarios\":[".len(),
        None => return out,
    };
    let bytes = s.as_bytes();
    let mut depth = 0i32;
    let mut obj_start = None;
    for i in arr_start..bytes.len() {
        match bytes[i] {
            b'{' => {
                if depth == 0 {
                    obj_start = Some(i);
                }
                depth += 1;
            }
            b'}' => {
                depth -= 1;
                if depth == 0
                    && let Some(st) = obj_start.take()
                {
                    out.push(&s[st..=i]);
                }
            }
            b']' if depth == 0 => break,
            _ => {}
        }
    }
    out
}

fn parse_row(obj: &str) -> SupervisorRow {
    SupervisorRow {
        name: json_str(obj, "name").unwrap_or_default(),
        state: ScenarioState::parse(&json_str(obj, "state").unwrap_or_default()),
        pid: json_int(obj, "pid").unwrap_or(0),
        cum_fills: json_int(obj, "cum_fills").unwrap_or(0),
        cum_shares: json_int(obj, "cum_shares").unwrap_or(0),
        last_fill_ts: json_int(obj, "last_fill_ts").unwrap_or(0),
        last_exit_reason: json_str(obj, "last_exit_reason").unwrap_or_default(),
        live: json_bool(obj, "live").unwrap_or(false),
    }
}

/// Parse one JSON-lines snapshot into `(ok, err, rows)`. A payload that is not a
/// JSON object (garbage/empty/partial) is an error rather than a panic.
pub fn parse_snapshot(line: &str) -> Result<(bool, String, Vec<SupervisorRow>), String> {
    let t = line.trim();
    if !t.starts_with('{') {
        return Err("not a JSON object".to_string());
    }
    let ok = json_bool(t, "ok").unwrap_or(false);
    let err = json_str(t, "err").unwrap_or_default();
    let rows = scenario_objects(t).iter().map(|o| parse_row(o)).collect();
    Ok((ok, err, rows))
}

/// Connect, write one request line, read the single reply line — all bounded by
/// [`IO_TIMEOUT`]. Never panics; every failure is an `Err(String)`.
async fn request(sock: &Path, line: &str) -> Result<String, String> {
    let fut = async {
        let mut stream = UnixStream::connect(sock).await.map_err(|e| e.to_string())?;
        stream
            .write_all(line.as_bytes())
            .await
            .map_err(|e| e.to_string())?;
        let mut buf = Vec::new();
        let mut chunk = [0u8; 4096];
        loop {
            let n = stream.read(&mut chunk).await.map_err(|e| e.to_string())?;
            if n == 0 {
                break;
            }
            if let Some(pos) = chunk[..n].iter().position(|b| *b == b'\n') {
                buf.extend_from_slice(&chunk[..pos]);
                break;
            }
            buf.extend_from_slice(&chunk[..n]);
            if buf.len() > 1 << 20 {
                break;
            }
        }
        Ok::<String, String>(String::from_utf8_lossy(&buf).to_string())
    };
    match tokio::time::timeout(IO_TIMEOUT, fut).await {
        Ok(r) => r,
        Err(_) => Err("timeout".to_string()),
    }
}

/// Poll the supervisor once. On any failure returns a `SupervisorState` with
/// `connected = false` and `last_error` set; never panics, never blocks the UI.
pub async fn poll_once(sock: &Path) -> SupervisorState {
    match request(sock, &list_request()).await {
        Ok(line) => match parse_snapshot(&line) {
            Ok((_ok, _err, rows)) => SupervisorState {
                connected: true,
                last_error: None,
                rows,
            },
            Err(e) => SupervisorState {
                connected: false,
                last_error: Some(format!("malformed reply: {e}")),
                ..Default::default()
            },
        },
        Err(e) => SupervisorState {
            connected: false,
            last_error: Some(e),
            ..Default::default()
        },
    }
}

/// Send one command line and return a human ok/err result the panel can show.
pub async fn send_command(sock: &Path, line: &str) -> String {
    match request(sock, line).await {
        Ok(reply) => match parse_snapshot(&reply) {
            Ok((true, _, _)) => "ok".to_string(),
            Ok((false, err, _)) => format!("error: {err}"),
            Err(e) => format!("malformed reply: {e}"),
        },
        Err(e) => format!("supervisor not connected: {e}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SNAPSHOT: &str = "{\"ok\":true,\"err\":\"\",\"scenarios\":[\
        {\"name\":\"0050\",\"state\":\"stopped\",\"pid\":0,\"cum_fills\":0,\"cum_shares\":0,\
        \"last_fill_ts\":0,\"last_exit_reason\":\"\",\"live\":false},\
        {\"name\":\"2330\",\"state\":\"in-window\",\"pid\":4242,\"cum_fills\":3,\"cum_shares\":3000,\
        \"last_fill_ts\":1700000000,\"last_exit_reason\":\"\",\"live\":true},\
        {\"name\":\"2454\",\"state\":\"closed-exited\",\"pid\":0,\"cum_fills\":5,\"cum_shares\":5000,\
        \"last_fill_ts\":1700000100,\"last_exit_reason\":\"window closed / run complete\",\"live\":false},\
        {\"name\":\"3008\",\"state\":\"crashed\",\"pid\":0,\"cum_fills\":0,\"cum_shares\":0,\
        \"last_fill_ts\":0,\"last_exit_reason\":\"order backend connect failed\",\"live\":true}]}\n";

    #[test]
    fn parses_all_states_and_fields() {
        let (ok, err, rows) = parse_snapshot(SNAPSHOT).unwrap();
        assert!(ok);
        assert!(err.is_empty());
        assert_eq!(rows.len(), 4);

        assert_eq!(rows[0].name, "0050");
        assert_eq!(rows[0].state, ScenarioState::Stopped);
        assert_eq!(rows[0].pid, 0);
        assert!(!rows[0].live);

        assert_eq!(rows[1].name, "2330");
        assert_eq!(rows[1].state, ScenarioState::InWindow);
        assert_eq!(rows[1].pid, 4242);
        assert_eq!(rows[1].cum_fills, 3);
        assert_eq!(rows[1].cum_shares, 3000);
        assert_eq!(rows[1].last_fill_ts, 1700000000);
        assert!(rows[1].live);

        assert_eq!(rows[2].state, ScenarioState::ClosedExited);
        assert_eq!(rows[2].last_exit_reason, "window closed / run complete");
        assert_eq!(rows[2].cum_fills, 5);

        assert_eq!(rows[3].state, ScenarioState::Crashed);
        assert_eq!(rows[3].last_exit_reason, "order backend connect failed");
    }

    #[test]
    fn running_classification() {
        for s in ["starting", "wait-open", "in-window", "fill-remainder"] {
            assert!(
                ScenarioState::parse(s).is_running(),
                "{s} should be running"
            );
        }
        for s in ["stopped", "stopping", "closed-exited", "crashed"] {
            assert!(
                !ScenarioState::parse(s).is_running(),
                "{s} should not be running"
            );
        }
    }

    #[test]
    fn unknown_state_is_surfaced_not_panicked() {
        let s = ScenarioState::parse("wat");
        assert_eq!(s, ScenarioState::Unknown("wat".to_string()));
        assert!(!s.is_running());
        assert_eq!(s.name(), "wat");
    }

    #[test]
    fn ok_false_error_reply_parses() {
        let line = "{\"ok\":false,\"err\":\"unknown scenario\",\"scenarios\":[]}\n";
        let (ok, err, rows) = parse_snapshot(line).unwrap();
        assert!(!ok);
        assert_eq!(err, "unknown scenario");
        assert!(rows.is_empty());
    }

    #[test]
    fn garbage_and_partial_are_err_not_panic() {
        assert!(parse_snapshot("").is_err());
        assert!(parse_snapshot("not json").is_err());
        assert!(parse_snapshot("[1,2,3]").is_err());
        // A truncated line still must not panic; it parses what it can.
        assert!(parse_snapshot("{\"ok\":true,\"err\":\"\",\"scenari").is_ok());
    }

    #[test]
    fn list_request_exact_bytes() {
        assert_eq!(list_request(), "{\"cmd\":\"list\"}\n");
    }

    #[test]
    fn start_request_tokens_and_escaping() {
        assert_eq!(
            start_request("2330", Mode::Paper),
            "{\"cmd\":\"start\",\"name\":\"2330\",\"mode\":\"paper\"}\n"
        );
        assert_eq!(
            start_request("2330", Mode::Live),
            "{\"cmd\":\"start\",\"name\":\"2330\",\"mode\":\"live\"}\n"
        );
        assert_eq!(
            start_request("2330", Mode::Test),
            "{\"cmd\":\"start\",\"name\":\"2330\",\"mode\":\"test\"}\n"
        );
        assert_eq!(
            start_request("a\"b\\c", Mode::Paper),
            "{\"cmd\":\"start\",\"name\":\"a\\\"b\\\\c\",\"mode\":\"paper\"}\n"
        );
    }

    #[test]
    fn stop_request_exact_bytes() {
        assert_eq!(
            stop_request("2330"),
            "{\"cmd\":\"stop\",\"name\":\"2330\"}\n"
        );
    }

    // CROSS-SIDE: these are the exact bytes emitted by the C++ SerializeScenario-
    // Snapshot (copied verbatim from a real emit run of the upgraded serializer)
    // for a last_exit_reason carrying newline, tab, quote, backslash, a 0x1c
    // control byte, and CJK. The upgraded TUI unescaper must recover the original.
    const CPP_HOSTILE_SNAPSHOT: &str = "{\"ok\":true,\"err\":\"\",\"scenarios\":[\
        {\"name\":\"2330\",\"state\":\"crashed\",\"pid\":0,\"cum_fills\":0,\"cum_shares\":0,\
        \"last_fill_ts\":0,\"last_exit_reason\":\"reject:\\nline2\\ttab \\\"q\\\" \
        back\\\\slash \\u001ctl 台積電\",\"live\":false,\"restart_count\":3,\"gave_up\":true}]}\n";

    #[test]
    fn cross_side_hostile_last_exit_reason_round_trips() {
        let (ok, err, rows) = parse_snapshot(CPP_HOSTILE_SNAPSHOT).unwrap();
        assert!(ok);
        assert!(err.is_empty());
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].name, "2330");
        assert_eq!(rows[0].state, ScenarioState::Crashed);
        assert_eq!(
            rows[0].last_exit_reason,
            "reject:\nline2\ttab \"q\" back\\slash \u{1c}tl 台積電"
        );
    }

    #[test]
    fn escape_unescape_round_trips_and_no_double_decode() {
        // Every escape our own emitter can produce survives our own unescaper.
        for hostile in [
            "a\nb",
            "tab\there",
            "q\"uote",
            "back\\slash",
            "cr\rlf",
            "form\u{0c}feed",
            "bell\u{07}end",
            "台積電 2330",
            // literal backslash-n must NOT decode to a newline (no double-decode).
            "literal\\n keep",
        ] {
            let line = stop_request(hostile);
            assert_eq!(
                json_str(&line, "name").as_deref(),
                Some(hostile),
                "{hostile:?}"
            );
        }
    }

    #[test]
    fn resolver_matches_socket_convention() {
        assert_eq!(
            resolve(Some("/run/ctl.sock"), Some("/run/user/1001"), None),
            Some("/run/ctl.sock".to_string())
        );
        assert_eq!(
            resolve(None, Some("/run/user/1001"), Some("/run/user/1001")),
            Some("/run/user/1001/kairos-scenario-ctl.sock".to_string())
        );
        assert_eq!(
            resolve(None, None, Some("/run/user/1001")),
            Some("/run/user/1001/kairos-scenario-ctl.sock".to_string())
        );
        assert_eq!(resolve(None, None, None), None);
        assert_eq!(resolve(Some(""), Some(""), Some("")), None);
    }

    #[test]
    fn socket_absent_degrades_without_panic() {
        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();
        let state = rt.block_on(poll_once(Path::new("/no/such/kairos-scenario-ctl.sock")));
        assert!(!state.connected);
        assert!(state.last_error.is_some());
        assert!(state.rows.is_empty());
    }
}
