//! Adversarial robustness probes for the supervisor client: a fake in-process
//! daemon returns malformed / partial / hung / oversized replies and we assert
//! the client degrades to not-connected WITHOUT panicking and WITHOUT exceeding
//! the IO timeout budget. No C++ needed.

use std::io::{Read, Write};
use std::os::unix::net::UnixListener;
use std::path::PathBuf;
use std::thread;
use std::time::{Duration, Instant};

use kairos_tui::sources::supervisor::{self, Mode};

fn tmp_sock(name: &str) -> PathBuf {
    let mut p = std::env::temp_dir();
    p.push(format!(
        "kairos-sup-robust-{}-{}.sock",
        std::process::id(),
        name
    ));
    let _ = std::fs::remove_file(&p);
    p
}

fn rt() -> tokio::runtime::Runtime {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap()
}

/// Spawn a one-shot fake daemon. `handler` gets the accepted stream (blocking
/// std socket) and does whatever the scenario needs. Returns the socket path.
fn fake_daemon<F>(name: &str, handler: F) -> PathBuf
where
    F: FnOnce(std::os::unix::net::UnixStream) + Send + 'static,
{
    let path = tmp_sock(name);
    let listener = UnixListener::bind(&path).unwrap();
    thread::spawn(move || {
        if let Ok((stream, _)) = listener.accept() {
            handler(stream);
        }
    });
    path
}

fn drain_request(stream: &mut std::os::unix::net::UnixStream) {
    // Read the one request line so our write is the reply to a real request.
    let mut buf = [0u8; 512];
    let _ = stream.read(&mut buf);
}

#[test]
fn connect_refused_when_path_exists_but_nothing_listens() {
    // A plain file (not a socket) at the path: connect must fail, not panic.
    let path = tmp_sock("plainfile");
    std::fs::write(&path, b"not a socket").unwrap();
    let st = rt().block_on(supervisor::poll_once(&path));
    assert!(!st.connected);
    assert!(st.last_error.is_some());
    assert!(st.rows.is_empty());
    let _ = std::fs::remove_file(&path);
}

#[test]
fn empty_reply_then_eof_is_not_connected() {
    let path = fake_daemon("eof", |mut s| {
        drain_request(&mut s);
        // close immediately: client sees EOF with empty buffer
        drop(s);
    });
    let st = rt().block_on(supervisor::poll_once(&path));
    assert!(!st.connected, "empty reply must be not-connected: {st:?}");
    let _ = std::fs::remove_file(&path);
}

#[test]
fn hung_daemon_times_out_within_budget_and_does_not_block_forever() {
    // Accept, read request, then hold the connection open forever without a reply.
    let path = fake_daemon("hung", |mut s| {
        drain_request(&mut s);
        thread::sleep(Duration::from_secs(30)); // hold fd open, no data
    });
    let start = Instant::now();
    let st = rt().block_on(supervisor::poll_once(&path));
    let elapsed = start.elapsed();
    assert!(!st.connected, "hung daemon must be not-connected");
    assert!(
        elapsed < Duration::from_millis(3500),
        "poll_once against a hung daemon took {elapsed:?}; must be bounded ~2s"
    );
    let _ = std::fs::remove_file(&path);
}

#[test]
fn valid_reply_returns_promptly_without_waiting_for_close() {
    // Send a valid snapshot line WITH newline but keep the connection open.
    let path = fake_daemon("valid-open", |mut s| {
        drain_request(&mut s);
        let line = "{\"ok\":true,\"err\":\"\",\"scenarios\":[\
            {\"name\":\"2330\",\"state\":\"in-window\",\"pid\":42,\"cum_fills\":1,\
            \"cum_shares\":1000,\"last_fill_ts\":0,\"last_exit_reason\":\"\",\"live\":false}]}\n";
        let _ = s.write_all(line.as_bytes());
        thread::sleep(Duration::from_secs(30)); // never close
    });
    let start = Instant::now();
    let st = rt().block_on(supervisor::poll_once(&path));
    let elapsed = start.elapsed();
    assert!(st.connected, "valid reply must be connected: {st:?}");
    assert_eq!(st.rows.len(), 1);
    assert_eq!(st.rows[0].name, "2330");
    assert!(
        elapsed < Duration::from_millis(1000),
        "a valid reply must return on the newline, not wait for close/timeout: {elapsed:?}"
    );
    let _ = std::fs::remove_file(&path);
}

#[test]
fn chunked_reply_is_reassembled() {
    let path = fake_daemon("chunked", |mut s| {
        drain_request(&mut s);
        let parts = [
            "{\"ok\":true,\"err\":\"\",\"scen",
            "arios\":[{\"name\":\"005",
            "0\",\"state\":\"stopped\",\"pid\":0,\"cum_fills\":0,\"cum_shares\":0,",
            "\"last_fill_ts\":0,\"last_exit_reason\":\"\",\"live\":false}]}\n",
        ];
        for p in parts {
            let _ = s.write_all(p.as_bytes());
            let _ = s.flush();
            thread::sleep(Duration::from_millis(50));
        }
        thread::sleep(Duration::from_secs(2));
    });
    let st = rt().block_on(supervisor::poll_once(&path));
    assert!(
        st.connected,
        "chunked valid reply must be connected: {st:?}"
    );
    assert_eq!(st.rows.len(), 1);
    assert_eq!(st.rows[0].name, "0050");
    let _ = std::fs::remove_file(&path);
}

#[test]
fn oversized_no_newline_reply_is_bounded_not_ooms() {
    // 3 MB of junk with no newline: client caps buffering at ~1MB and bails.
    let path = fake_daemon("oversized", |mut s| {
        drain_request(&mut s);
        let junk = vec![b'x'; 3 * 1024 * 1024];
        let _ = s.write_all(&junk);
        thread::sleep(Duration::from_secs(2));
    });
    let start = Instant::now();
    let st = rt().block_on(supervisor::poll_once(&path));
    let elapsed = start.elapsed();
    assert!(!st.connected, "garbage must be not-connected");
    assert!(
        elapsed < Duration::from_millis(3500),
        "oversized reply must be bounded: {elapsed:?}"
    );
    let _ = std::fs::remove_file(&path);
}

#[test]
fn malformed_json_with_newline_degrades() {
    let path = fake_daemon("malformed", |mut s| {
        drain_request(&mut s);
        let _ = s.write_all(b"totally not json at all\n");
        thread::sleep(Duration::from_millis(200));
    });
    let st = rt().block_on(supervisor::poll_once(&path));
    assert!(
        !st.connected,
        "malformed reply must be not-connected: {st:?}"
    );
    assert!(st.last_error.unwrap().contains("malformed"));
    let _ = std::fs::remove_file(&path);
}

#[test]
fn send_command_error_reply_is_surfaced() {
    let path = fake_daemon("cmderr", |mut s| {
        drain_request(&mut s);
        let _ = s.write_all(b"{\"ok\":false,\"err\":\"unknown scenario\",\"scenarios\":[]}\n");
        thread::sleep(Duration::from_millis(200));
    });
    let msg = rt().block_on(supervisor::send_command(
        &path,
        &supervisor::start_request("nope", Mode::Test),
    ));
    assert_eq!(msg, "error: unknown scenario", "got: {msg}");
    let _ = std::fs::remove_file(&path);
}

#[test]
fn send_command_absent_socket_reports_not_connected() {
    let path = tmp_sock("absent-cmd");
    let msg = rt().block_on(supervisor::send_command(
        &path,
        &supervisor::stop_request("2330"),
    ));
    assert!(msg.starts_with("supervisor not connected"), "got: {msg}");
}

#[test]
fn brace_inside_string_value_does_not_panic() {
    // The brace-depth splitter does not skip string contents; an embedded '}' in
    // a value would misparse — but it must NOT panic (never emit a false running).
    let path = fake_daemon("brace", |mut s| {
        drain_request(&mut s);
        let _ = s.write_all(
            b"{\"ok\":true,\"err\":\"\",\"scenarios\":[{\"name\":\"2330\",\"state\":\"stopped\",\
              \"pid\":0,\"cum_fills\":0,\"cum_shares\":0,\"last_fill_ts\":0,\
              \"last_exit_reason\":\"weird } reason { x\",\"live\":true}]}\n",
        );
        thread::sleep(Duration::from_millis(200));
    });
    let st = rt().block_on(supervisor::poll_once(&path));
    // Whatever it parses, it must be connected-without-panic and never running.
    assert!(st.connected);
    for r in &st.rows {
        assert!(
            !r.state.is_running(),
            "misparse must not fabricate a running state"
        );
    }
    let _ = std::fs::remove_file(&path);
}
