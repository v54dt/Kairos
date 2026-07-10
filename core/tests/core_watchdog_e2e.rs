//! Real end-to-end proofs for the 24/7 hardening, against the actual kairos-driver,
//! kairos-core and kairos-replayd binaries in an isolated Aeron dir (never the live
//! stack). Ignored by default because they spawn daemons.
//!
//! Run: cargo test --test core_watchdog_e2e -- --ignored --nocapture
//!
//! Proof A — SIGTERM kairos-core mid-stream: a connected UDS reader receives only
//! WHOLE length-prefixed frames, then a clean EOF, and core exits 0.
//!
//! Proof B — SIGKILL kairos-driver under a running core: core detects the dead
//! driver and exits NON-ZERO within a bounded window (no eternal spin).

use std::io::{ErrorKind, Read, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
use std::time::{Duration, Instant};

use kairos_core::encode::encode_subscribe;

const SYMBOL: &str = "2330";
const DRIVER_BIN: &str = env!("CARGO_BIN_EXE_kairos-driver");
const CORE_BIN: &str = env!("CARGO_BIN_EXE_kairos-core");
const REPLAYD_BIN: &str = env!("CARGO_BIN_EXE_kairos-replayd");

fn manifest() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

fn wait_for(path: &Path, timeout: Duration) -> bool {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if path.exists() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    false
}

/// Kills+reaps a spawned daemon on drop so a panic never orphans the pipeline.
struct Kid(Child);
impl Drop for Kid {
    fn drop(&mut self) {
        if matches!(self.0.try_wait(), Ok(None)) {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }
}

fn sigterm(child: &Child) {
    // SAFETY: signalling a child pid we own and have not yet reaped.
    unsafe { libc::kill(child.id() as i32, libc::SIGTERM) };
}

fn wait_exit(child: &mut Child, timeout: Duration) -> Option<std::process::ExitStatus> {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if let Ok(Some(status)) = child.try_wait() {
            return Some(status);
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    None
}

fn write_frame(s: &mut UnixStream, payload: &[u8]) {
    s.write_all(&(payload.len() as u32).to_le_bytes()).unwrap();
    s.write_all(payload).unwrap();
    s.flush().unwrap();
}

/// Reads one length-prefixed frame. Ok(None) is a clean EOF at a frame boundary; an
/// EOF after a partial length or partial payload is an Err — i.e. a cut frame.
fn read_frame(s: &mut UnixStream) -> std::io::Result<Option<Vec<u8>>> {
    let mut len_buf = [0u8; 4];
    let mut got = 0;
    while got < 4 {
        match s.read(&mut len_buf[got..]) {
            Ok(0) if got == 0 => return Ok(None),
            Ok(0) => {
                return Err(std::io::Error::new(
                    ErrorKind::UnexpectedEof,
                    "partial length prefix at cut",
                ));
            }
            Ok(n) => got += n,
            Err(e) => return Err(e),
        }
    }
    let len = u32::from_le_bytes(len_buf) as usize;
    let mut buf = vec![0u8; len];
    let mut got = 0;
    while got < len {
        match s.read(&mut buf[got..]) {
            Ok(0) => {
                return Err(std::io::Error::new(
                    ErrorKind::UnexpectedEof,
                    "frame truncated mid-payload",
                ));
            }
            Ok(n) => got += n,
            Err(e) => return Err(e),
        }
    }
    Ok(Some(buf))
}

struct Pipeline {
    aeron_dir: String,
    quote_sock: String,
    driver: Kid,
    core: Child,
    replayd: Option<Kid>,
    tape: PathBuf,
}

impl Pipeline {
    /// Bring up driver + core in an isolated Aeron dir. `driver_timeout_ms` tunes
    /// core's driver-liveness watchdog (small => fast proof).
    fn up(tag: &str, driver_timeout_ms: i64) -> Self {
        let aeron_dir = format!("/dev/shm/aeron-c8-{tag}");
        let _ = std::fs::remove_dir_all(&aeron_dir);
        let quote_sock = std::env::temp_dir()
            .join(format!("kairos-c8-{tag}.sock"))
            .to_string_lossy()
            .into_owned();
        let _ = std::fs::remove_file(&quote_sock);
        let tape = manifest().join("tests/fixtures/tapes/trend_day_2330.kqr");
        assert!(tape.is_file(), "tape fixture missing: {}", tape.display());

        let driver = Kid(Command::new(DRIVER_BIN)
            .env("KAIROS_AERON_DIR", &aeron_dir)
            .spawn()
            .expect("spawn kairos-driver"));
        assert!(
            wait_for(
                &Path::new(&aeron_dir).join("cnc.dat"),
                Duration::from_secs(15)
            ),
            "driver cnc.dat never appeared"
        );

        let core = Command::new(CORE_BIN)
            .env("KAIROS_AERON_DIR", &aeron_dir)
            .env("KAIROS_QUOTE_SOCK", &quote_sock)
            .env("KAIROS_DRIVER_TIMEOUT_MS", driver_timeout_ms.to_string())
            .spawn()
            .expect("spawn kairos-core");
        assert!(
            wait_for(Path::new(&quote_sock), Duration::from_secs(15)),
            "core quote socket never appeared"
        );

        Self {
            aeron_dir,
            quote_sock,
            driver,
            core,
            replayd: None,
            tape,
        }
    }

    fn start_replay(&mut self, speed: &str) {
        let replayd = Command::new(REPLAYD_BIN)
            .arg(&self.tape)
            .args([
                "--aeron-dir",
                &self.aeron_dir,
                "--pace",
                "accel",
                "--speed",
                speed,
            ])
            .spawn()
            .expect("spawn kairos-replayd");
        self.replayd = Some(Kid(replayd));
    }

    fn connect_subscribed(&self) -> UnixStream {
        let mut client = UnixStream::connect(&self.quote_sock).expect("connect quote socket");
        client
            .set_read_timeout(Some(Duration::from_secs(10)))
            .unwrap();
        write_frame(&mut client, &encode_subscribe(&[SYMBOL]));
        client
    }
}

impl Drop for Pipeline {
    fn drop(&mut self) {
        if matches!(self.core.try_wait(), Ok(None)) {
            let _ = self.core.kill();
            let _ = self.core.wait();
        }
        let _ = std::fs::remove_file(&self.quote_sock);
        let _ = std::fs::remove_dir_all(&self.aeron_dir);
    }
}

#[test]
#[ignore = "spawns kairos-driver + kairos-core + kairos-replayd; run with --ignored"]
fn sigterm_core_midstream_delivers_whole_frames_then_exits_zero() {
    let mut pipe = Pipeline::up(&format!("term-{}", std::process::id()), 10_000);
    pipe.start_replay("2");
    let mut client = pipe.connect_subscribed();

    // Confirm the stream is flowing, then stop reading so frames pile into the socket
    // buffer — now the cut lands with frames genuinely in flight.
    let before = read_frame(&mut client)
        .expect("frame before SIGTERM")
        .map(|_| 1)
        .unwrap_or(0);
    assert!(before > 0, "no frames flowed before SIGTERM");
    std::thread::sleep(Duration::from_millis(200));

    // Cut mid-stream.
    sigterm(&pipe.core);

    // Drain the rest: every read must be a whole frame (Ok(Some)) until a clean EOF.
    let mut after = 0;
    loop {
        match read_frame(&mut client) {
            Ok(Some(_)) => after += 1,
            Ok(None) => break,
            Err(e) => panic!("frame cut at shutdown: {e:?}"),
        }
    }
    println!("proof A: {before} frames before, {after} whole frames drained, then EOF");

    let status = wait_exit(&mut pipe.core, Duration::from_secs(15))
        .expect("core did not exit after SIGTERM");
    assert!(
        status.success(),
        "core must exit 0 on SIGTERM, got {status:?}"
    );
    println!("proof A: kairos-core exited {status:?} (whole frames only, clean shutdown)");
}

#[test]
#[ignore = "spawns kairos-driver + kairos-core; run with --ignored"]
fn killing_the_driver_makes_core_exit_nonzero_in_bounded_time() {
    let driver_timeout_ms = 2_000;
    let mut pipe = Pipeline::up(&format!("kill-{}", std::process::id()), driver_timeout_ms);
    pipe.start_replay("4");

    // Confirm core is fully up with a live client conductor before killing the driver.
    let mut client = pipe.connect_subscribed();
    assert!(
        read_frame(&mut client)
            .expect("frame before kill")
            .is_some(),
        "core produced no frame before the driver was killed"
    );
    // Settle so BOTH aeron clients (poll + control publisher) have fully connected
    // while the driver is alive. This isolates the driver-liveness watchdog as the
    // sole remaining exit path — the steady-state silent-spin scenario.
    std::thread::sleep(Duration::from_secs(3));

    // Hard-kill the media driver: core's client is now stale and poll would spin.
    // SAFETY: signalling a child pid we own.
    unsafe { libc::kill(pipe.driver.0.id() as i32, libc::SIGKILL) };
    let killed_at = Instant::now();

    // Detection window ~= driver_timeout + grace(3s) + 1s check granularity. Assert
    // core exits well within 2x that, i.e. no eternal spin.
    let bound = Duration::from_millis((driver_timeout_ms as u64 + 3_000 + 1_000) * 2 + 5_000);
    let status = wait_exit(&mut pipe.core, bound)
        .unwrap_or_else(|| panic!("core did NOT exit within {bound:?} after the driver died"));
    let took = killed_at.elapsed();
    assert!(
        !status.success(),
        "core must exit non-zero when the driver dies, got {status:?}"
    );
    println!("proof B: kairos-core exited {status:?} {took:?} after the driver was killed");
}
