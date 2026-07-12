//! Cross-language golden: schema/testdata/ctl_corpus.txt. SNAP<TAB> lines are the
//! C++ SerializeScenarioSnapshot outputs (parsed here via the real parse_snapshot);
//! REQ<TAB> lines are the request wire form, regenerated here from the real tui
//! builders and byte-compared (catches builder drift vs the C++ ParseScenarioRequest
//! that reads them). See schema/testdata/README.

use kairos_tui::sources::supervisor::{
    Mode, ScenarioState, list_request, parse_snapshot, start_request, stop_request,
};

const CORPUS: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../schema/testdata/ctl_corpus.txt"
));

fn tagged(tag: &str) -> Vec<&'static str> {
    CORPUS
        .lines()
        .filter(|l| !l.is_empty() && !l.starts_with('#'))
        .filter_map(|l| l.strip_prefix(tag)?.strip_prefix('\t'))
        .collect()
}

// The exact hostile inputs the C++ generator used (kHostile / kHostileName).
fn hostile_reason() -> String {
    "reject:\nline2\ttab \"q\" \\slash \u{1c} sep 台積".to_string()
}
fn hostile_name() -> String {
    "ev\"il\n\ttab\\x\u{1c}台".to_string()
}

#[test]
fn req_lines_match_tui_builders() {
    let reqs = tagged("REQ");
    let strip = |s: String| s.trim_end_matches('\n').to_string();
    let want: Vec<String> = vec![
        strip(list_request()),
        strip(start_request("2330-Buy", Mode::Paper)),
        strip(start_request("2330-Buy", Mode::Live)),
        strip(start_request("sim-x", Mode::Test)),
        strip(stop_request("2330-Buy")),
        strip(start_request(&hostile_name(), Mode::Paper)),
        strip(stop_request(&hostile_name())),
    ];
    assert_eq!(reqs.len(), want.len(), "REQ line count");
    for (got, w) in reqs.iter().zip(want.iter()) {
        assert_eq!(*got, w.as_str());
    }
}

#[test]
fn snap_lines_parse_via_real_parser() {
    let snaps = tagged("SNAP");
    assert_eq!(snaps.len(), 4, "SNAP line count");

    // Empty ok snapshot.
    let (ok, err, rows) = parse_snapshot(snaps[0]).unwrap();
    assert!(ok);
    assert_eq!(err, "");
    assert!(rows.is_empty());

    // All eight states, in order, with their scalar fields.
    let (ok, _err, rows) = parse_snapshot(snaps[1]).unwrap();
    assert!(ok);
    let states: Vec<ScenarioState> = rows.iter().map(|r| r.state.clone()).collect();
    assert_eq!(
        states,
        vec![
            ScenarioState::Stopped,
            ScenarioState::Starting,
            ScenarioState::WaitOpen,
            ScenarioState::InWindow,
            ScenarioState::FillRemainder,
            ScenarioState::ClosedExited,
            ScenarioState::Crashed,
            ScenarioState::Stopping,
        ]
    );
    assert_eq!(rows[3].name, "s-inwindow");
    assert_eq!(rows[3].pid, 333);
    assert_eq!(rows[3].cum_fills, 5);
    assert_eq!(rows[3].cum_shares, 5000);
    assert_eq!(rows[3].last_fill_ts, 1_700_000_000);
    assert!(rows[3].live);
    assert_eq!(rows[5].last_exit_reason, "window closed / run complete");
    assert_eq!(rows[6].last_exit_reason, "order backend connect failed");

    // Hostile last_exit_reason round-trips through the C++ escaper and this parser.
    // (restart_count/gave_up are absent from SupervisorRow -> scope limit, not asserted.)
    let (ok, _err, rows) = parse_snapshot(snaps[2]).unwrap();
    assert!(ok);
    assert_eq!(rows.len(), 1);
    assert_eq!(rows[0].name, "s-give");
    assert_eq!(rows[0].last_exit_reason, hostile_reason());
    assert!(rows[0].live);

    // Error snapshot.
    let (ok, err, rows) = parse_snapshot(snaps[3]).unwrap();
    assert!(!ok);
    assert_eq!(err, "unknown cmd");
    assert!(rows.is_empty());
}
