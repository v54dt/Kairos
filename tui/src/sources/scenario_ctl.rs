use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use crate::sources::halt::HaltKey;

/// One scenario `.toml` discovered on disk, parsed just enough to drive the
/// start UI: its name/symbol for display and whether it launches LIVE.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ScenarioToml {
    pub path: PathBuf,
    pub name: String,
    pub symbol: String,
    pub live: bool,
}

impl ScenarioToml {
    /// A toml is a launchable scenario only when it carried a `[scenario]`
    /// section with a non-empty `symbol` (the only source of `symbol`). A
    /// credentials/config toml such as `hub.toml` has none, so it is never
    /// offered for launch as a real-money trader.
    pub fn is_launchable(&self) -> bool {
        !self.symbol.is_empty()
    }
}

/// The launch mode resolved for a start. LIVE submits real-money orders and
/// demands the typed confirm; PAPER is a simulated fill run and is y/N.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Launch {
    Live,
    Paper,
}

/// Resolve a toml's `[mode] live` into a launch mode. Any ambiguity is already
/// collapsed to `live = true` by the parser, so a `true` flag => LIVE.
pub fn classify_launch(live: bool) -> Launch {
    if live { Launch::Live } else { Launch::Paper }
}

/// The `.toml` file stem (name without extension) — what the operator types to
/// confirm a LIVE start. Falls back to the whole file name if it has no stem.
pub fn toml_stem(path: &Path) -> String {
    path.file_stem()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_default()
}

/// Strip a TOML scalar value to its content: a quoted string keeps its inner
/// text; a bare value drops any trailing `# comment`.
fn clean_value(val: &str) -> String {
    let val = val.trim();
    if let Some(rest) = val.strip_prefix('"') {
        return match rest.find('"') {
            Some(end) => rest[..end].to_string(),
            None => rest.to_string(),
        };
    }
    val.split('#').next().unwrap_or("").trim().to_string()
}

/// Section-aware line scan of a scenario `.toml`. Reads `name`/`symbol` under
/// `[scenario]` and `live` under `[mode]`. FAIL-SAFE: if `[mode] live` is
/// absent or not exactly `true`/`false`, the scenario is treated as LIVE, so an
/// unreadable mode never takes the easy paper path. Total: never panics.
pub fn parse_scenario_toml(path: PathBuf, text: &str) -> ScenarioToml {
    let mut section = String::new();
    let mut name = String::new();
    let mut symbol = String::new();
    let mut live: Option<bool> = None;
    for raw in text.lines() {
        let line = raw.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if line.starts_with('[') && line.ends_with(']') {
            section = line[1..line.len() - 1].trim().to_string();
            continue;
        }
        let Some((key, val)) = line.split_once('=') else {
            continue;
        };
        let key = key.trim();
        let val = clean_value(val);
        match (section.as_str(), key) {
            ("scenario", "name") => name = val,
            ("scenario", "symbol") => symbol = val,
            ("mode", "live") => {
                live = match val.as_str() {
                    "true" => Some(true),
                    "false" => Some(false),
                    _ => None,
                }
            }
            _ => {}
        }
    }
    ScenarioToml {
        path,
        name,
        symbol,
        live: live.unwrap_or(true),
    }
}

/// Enumerate the available scenario tomls in `dir`. Skips `*.example.*`
/// templates, non-`.toml` files, and any toml that is not a launchable scenario
/// (no non-empty `[scenario] symbol`, e.g. a credentials `hub.toml`); an
/// unreadable or non-launchable toml is counted in the returned `skipped` total
/// and left out rather than crashing the scan or being offered for launch.
pub fn enumerate_available(dir: &Path) -> (Vec<ScenarioToml>, usize) {
    let mut out = Vec::new();
    let mut skipped = 0usize;
    let entries = match std::fs::read_dir(dir) {
        Ok(e) => e,
        Err(_) => return (out, skipped),
    };
    for entry in entries.flatten() {
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if !name.ends_with(".toml") || name.contains(".example.") {
            continue;
        }
        match std::fs::read_to_string(entry.path()) {
            Ok(text) => {
                let scen = parse_scenario_toml(entry.path(), &text);
                if scen.is_launchable() {
                    out.push(scen);
                } else {
                    skipped += 1;
                }
            }
            Err(_) => skipped += 1,
        }
    }
    out.sort_by(|a, b| a.path.cmp(&b.path));
    (out, skipped)
}

/// The scenario-trader binary file name. It is the leaf of the derived default
/// path (`<scenario_dir>/build/kairos_scenario_trader`) and the argv[0] basename
/// a running trader is matched by.
pub const TRADER_BIN: &str = "kairos_scenario_trader";

/// A running `kairos_scenario_trader` process, seen by scanning the process
/// table. Catches both paper and live traders (only live ones reach the hub).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RunningTrader {
    pub pid: i32,
    pub toml: String,
    pub live: bool,
}

/// The configured trader binary basename: `$KAIROS_SCENARIO_TRADER`'s basename
/// if set, else [`TRADER_BIN`]. This is what a running argv[0] must match.
fn trader_basename() -> String {
    std::env::var("KAIROS_SCENARIO_TRADER")
        .ok()
        .and_then(|p| {
            Path::new(&p)
                .file_name()
                .map(|s| s.to_string_lossy().to_string())
        })
        .unwrap_or_else(|| TRADER_BIN.to_string())
}

fn basename(arg: &str) -> &str {
    arg.rsplit(['/']).next().unwrap_or(arg)
}

/// Classify one process's argv. Returns `Some` only when argv[0]'s basename is
/// the scenario trader AND a `.toml` argument is present; `live` is set when the
/// argv carries `--live`. A foreign process or a trader with no toml is `None`.
pub fn parse_trader_cmdline(pid: i32, argv: &[String]) -> Option<RunningTrader> {
    let arg0 = argv.first()?;
    if basename(arg0) != trader_basename() {
        return None;
    }
    let toml = argv
        .iter()
        .skip(1)
        .find(|a| !a.starts_with('-') && a.ends_with(".toml"))?
        .clone();
    let live = argv.iter().any(|a| a == "--live");
    Some(RunningTrader { pid, toml, live })
}

/// Read `/proc/<pid>/cmdline` (NUL-separated) into an argv vector. `None` when
/// the process is gone or the file is unreadable/empty.
fn read_cmdline(pid: i32) -> Option<Vec<String>> {
    let raw = std::fs::read(format!("/proc/{pid}/cmdline")).ok()?;
    if raw.is_empty() {
        return None;
    }
    let argv: Vec<String> = raw
        .split(|b| *b == 0)
        .filter(|s| !s.is_empty())
        .map(|s| String::from_utf8_lossy(s).to_string())
        .collect();
    (!argv.is_empty()).then_some(argv)
}

/// Scan `/proc` for running scenario traders. Skips non-numeric and unreadable
/// entries; never panics on a racing/vanishing process.
pub fn enumerate_running() -> Vec<RunningTrader> {
    let mut out = Vec::new();
    let entries = match std::fs::read_dir("/proc") {
        Ok(e) => e,
        Err(_) => return out,
    };
    for entry in entries.flatten() {
        let name = entry.file_name();
        let Ok(pid) = name.to_string_lossy().parse::<i32>() else {
            continue;
        };
        if let Some(argv) = read_cmdline(pid)
            && let Some(t) = parse_trader_cmdline(pid, &argv)
        {
            out.push(t);
        }
    }
    out.sort_by(|a, b| a.pid.cmp(&b.pid));
    out
}

/// Re-validate that `pid` is still a scenario trader immediately before
/// signalling it — closes the poll-to-action pid-reuse window so a STOP can
/// never SIGINT an arbitrary process.
pub fn is_scenario_trader_pid(pid: i32) -> bool {
    read_cmdline(pid)
        .and_then(|argv| parse_trader_cmdline(pid, &argv))
        .is_some()
}

/// One unified Scenarios-panel row: an available scenario, a running trader, or
/// both (a running trader matched to its toml). `avail` drives START; `running`
/// drives STOP. An orphan trader (started elsewhere, no toml on disk) is a row
/// with `avail: None` so it stays stoppable.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ScenarioRow {
    pub stem: String,
    pub symbol: String,
    pub live: bool,
    pub avail: Option<ScenarioToml>,
    pub running: Option<RunningTrader>,
}

impl ScenarioRow {
    pub fn is_running(&self) -> bool {
        self.running.is_some()
    }

    pub fn pid(&self) -> Option<i32> {
        self.running.as_ref().map(|t| t.pid)
    }
}

/// Merge available scenarios with running traders into one row list. Each
/// available scenario becomes a row, attached to at most one running trader that
/// shares its toml file stem. INVARIANT: every running trader is its own
/// stoppable row — traders are consumed BY PID, so two traders sharing a stem
/// (e.g. a daily restart where the old pid is still in SIGINT wind-down) never
/// collapse: the first fills the scenario's row, every still-unconsumed trader
/// (same-stem duplicate or no on-disk toml) is appended as its own orphan row.
/// Available scenarios come first in their sorted order.
pub fn merge_rows(avail: &[ScenarioToml], running: &[RunningTrader]) -> Vec<ScenarioRow> {
    let run_stem = |t: &RunningTrader| toml_stem(Path::new(&t.toml));
    let mut consumed: Vec<i32> = Vec::new();
    let mut rows = Vec::new();
    for s in avail {
        let stem = toml_stem(&s.path);
        let run = running
            .iter()
            .find(|t| run_stem(t) == stem && !consumed.contains(&t.pid))
            .cloned();
        if let Some(t) = &run {
            consumed.push(t.pid);
        }
        rows.push(ScenarioRow {
            stem,
            symbol: s.symbol.clone(),
            live: run.as_ref().map(|t| t.live).unwrap_or(s.live),
            avail: Some(s.clone()),
            running: run,
        });
    }
    for t in running {
        if consumed.contains(&t.pid) {
            continue;
        }
        rows.push(ScenarioRow {
            stem: run_stem(t),
            symbol: String::new(),
            live: t.live,
            avail: None,
            running: Some(t.clone()),
        });
    }
    rows
}

/// An action authorized by a completed confirmation. `Start` carries the launch
/// mode captured at confirm time, so a paper confirm can never emit a live start.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ScenarioAction {
    Start { toml: PathBuf, launch: Launch },
    Stop { pid: i32 },
}

/// Confirmation prompt state. A LIVE start requires typing the toml stem then
/// Enter (the sole real-money gate for a detached trader); a PAPER start and any
/// STOP are a single y/N. `Idle` means nothing is pending. Each variant binds the
/// exact target shown so the operator cannot confirm a different scenario.
#[derive(Clone, Debug, PartialEq, Eq, Default)]
pub enum ScenarioPrompt {
    #[default]
    Idle,
    TypedStart {
        toml: PathBuf,
        name: String,
        symbol: String,
        stem: String,
        buf: String,
    },
    SimpleStart {
        toml: PathBuf,
        name: String,
        symbol: String,
    },
    SimpleStop {
        pid: i32,
        toml: String,
        live: bool,
    },
}

impl ScenarioPrompt {
    pub fn is_active(&self) -> bool {
        !matches!(self, ScenarioPrompt::Idle)
    }
}

/// Open a start confirmation for a scenario. Routes through [`classify_launch`]
/// so a LIVE toml always gets the typed confirm and a PAPER toml the y/N — a
/// single key can never spawn a live trader. An empty toml stem yields no
/// startable prompt (`Idle`), so a bare Enter can never confirm a LIVE start
/// against an empty required buffer.
pub fn begin_start(s: &ScenarioToml) -> ScenarioPrompt {
    match classify_launch(s.live) {
        Launch::Live => {
            let stem = toml_stem(&s.path);
            if stem.is_empty() {
                return ScenarioPrompt::Idle;
            }
            ScenarioPrompt::TypedStart {
                toml: s.path.clone(),
                name: s.name.clone(),
                symbol: s.symbol.clone(),
                stem,
                buf: String::new(),
            }
        }
        Launch::Paper => ScenarioPrompt::SimpleStart {
            toml: s.path.clone(),
            name: s.name.clone(),
            symbol: s.symbol.clone(),
        },
    }
}

/// Open a stop confirmation for a running trader. A stop only ever reduces
/// exposure, so it is always a y/N.
pub fn begin_stop(t: &RunningTrader) -> ScenarioPrompt {
    ScenarioPrompt::SimpleStop {
        pid: t.pid,
        toml: t.toml.clone(),
        live: t.live,
    }
}

/// Advance the confirm state machine. A typed start fires only on Enter with the
/// buffer equal to the toml stem; a simple start/stop fires only on 'y'/'Y'.
/// Anything else, or Cancel (Esc), returns to `Idle` without acting.
pub fn handle_key(
    prompt: &ScenarioPrompt,
    key: HaltKey,
) -> (ScenarioPrompt, Option<ScenarioAction>) {
    match prompt {
        ScenarioPrompt::Idle => (ScenarioPrompt::Idle, None),
        ScenarioPrompt::TypedStart {
            toml,
            name,
            symbol,
            stem,
            buf,
        } => match key {
            HaltKey::Cancel => (ScenarioPrompt::Idle, None),
            HaltKey::Enter => {
                if buf == stem {
                    (
                        ScenarioPrompt::Idle,
                        Some(ScenarioAction::Start {
                            toml: toml.clone(),
                            launch: Launch::Live,
                        }),
                    )
                } else {
                    (ScenarioPrompt::Idle, None)
                }
            }
            HaltKey::Backspace => {
                let mut b = buf.clone();
                b.pop();
                (rebuild_typed(toml, name, symbol, stem, b), None)
            }
            HaltKey::Char(c) => {
                let mut b = buf.clone();
                b.push(c);
                (rebuild_typed(toml, name, symbol, stem, b), None)
            }
        },
        ScenarioPrompt::SimpleStart { toml, .. } => match key {
            HaltKey::Char('y') | HaltKey::Char('Y') => (
                ScenarioPrompt::Idle,
                Some(ScenarioAction::Start {
                    toml: toml.clone(),
                    launch: Launch::Paper,
                }),
            ),
            _ => (ScenarioPrompt::Idle, None),
        },
        ScenarioPrompt::SimpleStop { pid, .. } => match key {
            HaltKey::Char('y') | HaltKey::Char('Y') => (
                ScenarioPrompt::Idle,
                Some(ScenarioAction::Stop { pid: *pid }),
            ),
            _ => (ScenarioPrompt::Idle, None),
        },
    }
}

fn rebuild_typed(toml: &Path, name: &str, symbol: &str, stem: &str, buf: String) -> ScenarioPrompt {
    ScenarioPrompt::TypedStart {
        toml: toml.to_path_buf(),
        name: name.to_string(),
        symbol: symbol.to_string(),
        stem: stem.to_string(),
        buf,
    }
}

/// Resolve the scenario-trader executable to spawn, in precedence order:
/// (a) the `--trader-bin` flag if given non-empty, else (b)
/// `$KAIROS_SCENARIO_TRADER` if set non-empty, else (c) a path DERIVED from the
/// scenario dir: `<scenario_dir>/build/kairos_scenario_trader` (the tomls and the
/// built binary are colocated under `exec/scenario`). The default is a real
/// derived path, never a bare PATH name — so a START works out of the box and
/// never fails with a raw ENOENT from a missing PATH lookup.
pub fn resolve_trader_bin(flag: Option<&str>, env: Option<&str>, scenario_dir: &Path) -> PathBuf {
    if let Some(f) = flag.filter(|s| !s.is_empty()) {
        return PathBuf::from(f);
    }
    if let Some(e) = env.filter(|s| !s.is_empty()) {
        return PathBuf::from(e);
    }
    scenario_dir.join("build").join(TRADER_BIN)
}

/// Verify the resolved trader binary is a runnable file before spawning, turning
/// a would-be raw `os error 2` into a clear, actionable message.
pub fn ensure_trader_bin(bin: &Path) -> Result<(), String> {
    let not_found = || {
        format!(
            "trader binary not found: {} (set --trader-bin or KAIROS_SCENARIO_TRADER)",
            bin.display()
        )
    };
    let meta = std::fs::metadata(bin).map_err(|_| not_found())?;
    if !meta.is_file() {
        return Err(not_found());
    }
    use std::os::unix::fs::PermissionsExt;
    if meta.permissions().mode() & 0o111 == 0 {
        return Err(format!(
            "trader binary is not executable: {} (set --trader-bin or KAIROS_SCENARIO_TRADER)",
            bin.display()
        ));
    }
    Ok(())
}

/// The exact argv for a start. PAPER is just `[bin, toml]`; LIVE appends
/// `--live --yes` — `--yes` skips the trader's stdin "type LIVE" gate, which a
/// detached process (no controlling terminal) could never answer, making the
/// TUI's typed confirm the sole real-money gate. Only the LIVE path adds `--yes`.
pub fn build_spawn_argv(bin: &str, toml_path: &Path, launch: Launch) -> Vec<String> {
    let mut argv = vec![bin.to_string(), toml_path.to_string_lossy().to_string()];
    if launch == Launch::Live {
        argv.push("--live".to_string());
        argv.push("--yes".to_string());
    }
    argv
}

/// Spawn a scenario trader fully DETACHED so it survives the TUI exiting:
/// `setsid` drops the controlling terminal (no SIGHUP) and a second fork
/// reparents the trader to init; the intermediate is reaped so no zombie
/// remains. Stdio goes to /dev/null. Returns a human launched/failed line.
pub fn spawn_detached(argv: &[String]) -> Result<String, String> {
    let bin = argv.first().ok_or_else(|| "empty argv".to_string())?;
    let mut cmd = Command::new(bin);
    cmd.args(&argv[1..])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    // SAFETY: only async-signal-safe libc calls run in the forked child.
    unsafe {
        cmd.pre_exec(|| {
            if libc::setsid() == -1 {
                return Err(std::io::Error::last_os_error());
            }
            match libc::fork() {
                -1 => Err(std::io::Error::last_os_error()),
                0 => Ok(()),
                _ => libc::_exit(0),
            }
        });
    }
    match cmd.spawn() {
        Ok(mut child) => {
            let _ = child.wait();
            Ok(format!("launched: {}", argv.join(" ")))
        }
        Err(e) => Err(format!("spawn failed: {e}")),
    }
}

/// The signal a STOP may send: `SIGINT` (graceful wind-down) only when the pid
/// was validated as a scenario trader, else `None` — never SIGKILL, never an
/// unvalidated pid.
pub fn stop_signal(is_trader: bool) -> Option<i32> {
    is_trader.then_some(libc::SIGINT)
}

/// Stop a trader: re-`validate` the pid immediately before signalling (closing
/// the poll-to-action pid-reuse window), then send exactly one SIGINT via
/// `signal`. `validate`/`signal` are injected so tests never touch a real
/// process; a pid that fails validation is refused and never signalled.
pub fn run_stop(
    pid: i32,
    validate: impl Fn(i32) -> bool,
    mut signal: impl FnMut(i32, i32) -> i32,
) -> Result<String, String> {
    match stop_signal(validate(pid)) {
        Some(sig) => {
            if signal(pid, sig) == 0 {
                Ok(format!("sent SIGINT to pid {pid}"))
            } else {
                Err(format!("kill pid {pid} failed"))
            }
        }
        None => Err(format!("pid {pid} is not a scenario trader")),
    }
}

/// Stop a running trader by pid, validating via `/proc` and sending SIGINT.
pub fn stop_trader(pid: i32) -> Result<String, String> {
    run_stop(pid, is_scenario_trader_pid, |pid, sig| unsafe {
        libc::kill(pid, sig)
    })
}

/// Read-only state the Scenarios panel needs to render process control. `sel`
/// is a single cursor over the merged row list.
#[derive(Clone, Debug, Default)]
pub struct ScenarioUi {
    pub sel: usize,
    pub confirm: ScenarioPrompt,
    pub last_result: Option<String>,
}

#[cfg(test)]
mod tests {
    use super::*;

    const LIVE_TOML: &str = "\
[scenario]
name = \"2330-accumulate\"
symbol = \"2330\"

[mode]
live = true
";

    const PAPER_TOML: &str = "\
[scenario]
name = \"0050-twap\"
symbol = \"0050\"  # ETF

[mode]
live = false
";

    // A credentials/config toml: has `[user]`, no `[scenario]`, no `[mode]`.
    const HUB_TOML: &str = "\
[user]
account = \"12345\"
password = \"secret\"
";

    // A real scenario (has a `[scenario]` symbol) whose `[mode] live` is garbled.
    const GARBLED_LIVE_SCENARIO: &str = "\
[scenario]
name = \"2454-accumulate\"
symbol = \"2454\"

[mode]
live = maybe
";

    #[test]
    fn parses_live_scenario() {
        let s = parse_scenario_toml(PathBuf::from("/x/2330.toml"), LIVE_TOML);
        assert_eq!(s.name, "2330-accumulate");
        assert_eq!(s.symbol, "2330");
        assert!(s.live);
        assert_eq!(classify_launch(s.live), Launch::Live);
    }

    #[test]
    fn parses_paper_scenario_and_strips_inline_comment() {
        let s = parse_scenario_toml(PathBuf::from("/x/0050.toml"), PAPER_TOML);
        assert_eq!(s.name, "0050-twap");
        assert_eq!(s.symbol, "0050");
        assert!(!s.live);
        assert_eq!(classify_launch(s.live), Launch::Paper);
    }

    #[test]
    fn missing_mode_is_live_failsafe() {
        let s = parse_scenario_toml(
            PathBuf::from("/x/nomode.toml"),
            "[scenario]\nname = \"n\"\nsymbol = \"s\"\n",
        );
        assert!(s.live);
        assert_eq!(classify_launch(s.live), Launch::Live);
    }

    #[test]
    fn garbled_mode_is_live_failsafe() {
        let s = parse_scenario_toml(PathBuf::from("/x/garble.toml"), "[mode]\nlive = maybe\n");
        assert!(s.live);
    }

    #[test]
    fn garbage_text_does_not_panic() {
        let s = parse_scenario_toml(PathBuf::from("/x/junk.toml"), "\0not [ = toml ]]]");
        // Fail-safe default holds on garbage.
        assert!(s.live);
    }

    #[test]
    fn stem_is_file_name_without_extension() {
        assert_eq!(toml_stem(Path::new("/a/b/2330.toml")), "2330");
        assert_eq!(toml_stem(Path::new("plain")), "plain");
    }

    #[test]
    fn enumerate_skips_examples_and_counts_unreadable() {
        let dir = std::env::temp_dir().join(format!("kairos-scen-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("2330.toml"), LIVE_TOML).unwrap();
        std::fs::write(dir.join("0050.toml"), PAPER_TOML).unwrap();
        std::fs::write(dir.join("scenario.example.toml"), LIVE_TOML).unwrap();
        std::fs::write(dir.join("notes.txt"), "ignore me").unwrap();
        std::fs::write(dir.join("hub.toml"), HUB_TOML).unwrap();
        // An unreadable toml: a directory named like one.
        std::fs::create_dir_all(dir.join("broken.toml")).unwrap();

        let (found, skipped) = enumerate_available(&dir);
        let names: Vec<_> = found.iter().map(|s| s.symbol.clone()).collect();
        assert!(names.contains(&"2330".to_string()));
        assert!(names.contains(&"0050".to_string()));
        assert_eq!(
            found.len(),
            2,
            "example + txt + hub excluded, broken skipped"
        );
        assert_eq!(skipped, 2, "broken dir + hub.toml (no [scenario] symbol)");

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn hub_style_toml_is_not_launchable() {
        let s = parse_scenario_toml(PathBuf::from("/e/hub.toml"), HUB_TOML);
        assert!(s.symbol.is_empty());
        assert!(!s.is_launchable(), "a credentials toml is not launchable");
    }

    #[test]
    fn real_scenarios_are_launchable() {
        assert!(parse_scenario_toml(PathBuf::from("/e/2330.toml"), LIVE_TOML).is_launchable());
        assert!(parse_scenario_toml(PathBuf::from("/e/0050.toml"), PAPER_TOML).is_launchable());
    }

    #[test]
    fn garbled_mode_real_scenario_stays_live_and_launchable() {
        let s = parse_scenario_toml(PathBuf::from("/e/2454.toml"), GARBLED_LIVE_SCENARIO);
        assert!(s.is_launchable());
        assert!(
            s.live,
            "garbled [mode] on a real scenario stays LIVE fail-safe"
        );
        assert_eq!(classify_launch(s.live), Launch::Live);
    }

    #[test]
    fn enumerate_excludes_hub_style_credentials_toml() {
        let dir = std::env::temp_dir().join(format!("kairos-scen-hub-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("2330.toml"), LIVE_TOML).unwrap();
        std::fs::write(dir.join("hub.toml"), HUB_TOML).unwrap();

        let (found, skipped) = enumerate_available(&dir);
        let names: Vec<_> = found.iter().map(|s| s.symbol.clone()).collect();
        assert_eq!(found.len(), 1, "only the real scenario is listed");
        assert_eq!(names, vec!["2330".to_string()]);
        assert!(
            found.iter().all(|s| s.path.file_stem().unwrap() != "hub"),
            "hub.toml (no [scenario]) must never appear in the launchable list"
        );
        assert_eq!(skipped, 1, "hub.toml counted as skipped, not listed");

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn enumerate_missing_dir_is_empty() {
        let (found, skipped) = enumerate_available(Path::new("/no/such/dir/kairos"));
        assert!(found.is_empty());
        assert_eq!(skipped, 0);
    }

    fn argv(v: &[&str]) -> Vec<String> {
        v.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn parse_cmdline_bare_argv0_paper() {
        let t = parse_trader_cmdline(
            42,
            &argv(&["kairos_scenario_trader", "/e/2330.toml", "--budget", "5"]),
        )
        .unwrap();
        assert_eq!(t.pid, 42);
        assert_eq!(t.toml, "/e/2330.toml");
        assert!(!t.live);
    }

    #[test]
    fn parse_cmdline_full_path_argv0_live() {
        let t = parse_trader_cmdline(
            7,
            &argv(&[
                "/opt/exec/build/kairos_scenario_trader",
                "/e/2330.toml",
                "--live",
                "--yes",
            ]),
        )
        .unwrap();
        assert_eq!(t.pid, 7);
        assert_eq!(t.toml, "/e/2330.toml");
        assert!(t.live);
    }

    #[test]
    fn parse_cmdline_rejects_foreign_argv0() {
        assert!(parse_trader_cmdline(1, &argv(&["bash", "/e/2330.toml"])).is_none());
        assert!(parse_trader_cmdline(1, &argv(&["vim", "2330.toml"])).is_none());
    }

    #[test]
    fn parse_cmdline_rejects_trader_without_toml() {
        assert!(parse_trader_cmdline(1, &argv(&["kairos_scenario_trader", "--help"])).is_none());
        assert!(parse_trader_cmdline(1, &argv(&["kairos_scenario_trader"])).is_none());
    }

    #[test]
    fn parse_cmdline_empty_argv_is_none() {
        assert!(parse_trader_cmdline(1, &[]).is_none());
    }

    #[test]
    fn own_process_is_not_a_trader() {
        // The test binary is not kairos_scenario_trader.
        assert!(!is_scenario_trader_pid(std::process::id() as i32));
    }

    fn live_scen() -> ScenarioToml {
        parse_scenario_toml(PathBuf::from("/e/2330.toml"), LIVE_TOML)
    }

    fn paper_scen() -> ScenarioToml {
        parse_scenario_toml(PathBuf::from("/e/0050.toml"), PAPER_TOML)
    }

    fn feed(mut p: ScenarioPrompt, s: &str) -> ScenarioPrompt {
        for c in s.chars() {
            p = handle_key(&p, HaltKey::Char(c)).0;
        }
        p
    }

    #[test]
    fn live_start_is_typed_confirm() {
        assert!(matches!(
            begin_start(&live_scen()),
            ScenarioPrompt::TypedStart { .. }
        ));
    }

    #[test]
    fn paper_start_is_simple_confirm() {
        assert!(matches!(
            begin_start(&paper_scen()),
            ScenarioPrompt::SimpleStart { .. }
        ));
    }

    #[test]
    fn ambiguous_toml_start_is_typed_confirm() {
        // Missing [mode] => live default => typed confirm (fail-safe).
        let ambiguous = parse_scenario_toml(PathBuf::from("/e/mystery.toml"), "[scenario]\n");
        assert!(matches!(
            begin_start(&ambiguous),
            ScenarioPrompt::TypedStart { .. }
        ));
    }

    #[test]
    fn empty_stem_live_start_is_not_startable() {
        // A path with no file stem yields an empty stem; a LIVE toml must then
        // produce no startable prompt, so a bare Enter can never fire a start.
        let s = parse_scenario_toml(PathBuf::from(""), LIVE_TOML);
        assert!(toml_stem(&s.path).is_empty());
        assert!(s.live);
        let p = begin_start(&s);
        assert_eq!(p, ScenarioPrompt::Idle);
        let (next, action) = handle_key(&p, HaltKey::Enter);
        assert_eq!(
            action, None,
            "a bare Enter must never confirm an empty-stem LIVE start"
        );
        assert_eq!(next, ScenarioPrompt::Idle);
    }

    #[test]
    fn typed_start_fires_only_on_stem_and_enter() {
        let p = feed(begin_start(&live_scen()), "2330");
        let (next, action) = handle_key(&p, HaltKey::Enter);
        assert_eq!(
            action,
            Some(ScenarioAction::Start {
                toml: PathBuf::from("/e/2330.toml"),
                launch: Launch::Live,
            })
        );
        assert_eq!(next, ScenarioPrompt::Idle);
    }

    #[test]
    fn typed_start_wrong_buffer_cancels_no_action() {
        let p = feed(begin_start(&live_scen()), "233");
        let (next, action) = handle_key(&p, HaltKey::Enter);
        assert_eq!(action, None);
        assert_eq!(next, ScenarioPrompt::Idle);
    }

    #[test]
    fn typed_start_esc_cancels_no_action() {
        let p = feed(begin_start(&live_scen()), "2330");
        let (next, action) = handle_key(&p, HaltKey::Cancel);
        assert_eq!(action, None);
        assert_eq!(next, ScenarioPrompt::Idle);
    }

    #[test]
    fn typed_start_backspace_edits() {
        let p = feed(begin_start(&live_scen()), "23X");
        match handle_key(&p, HaltKey::Backspace).0 {
            ScenarioPrompt::TypedStart { buf, .. } => assert_eq!(buf, "23"),
            other => panic!("expected TypedStart, got {other:?}"),
        }
    }

    #[test]
    fn paper_start_fires_only_on_y() {
        for c in ['y', 'Y'] {
            let (next, action) = handle_key(&begin_start(&paper_scen()), HaltKey::Char(c));
            assert_eq!(
                action,
                Some(ScenarioAction::Start {
                    toml: PathBuf::from("/e/0050.toml"),
                    launch: Launch::Paper,
                })
            );
            assert_eq!(next, ScenarioPrompt::Idle);
        }
        for key in [
            HaltKey::Char('n'),
            HaltKey::Char('x'),
            HaltKey::Enter,
            HaltKey::Cancel,
        ] {
            let (next, action) = handle_key(&begin_start(&paper_scen()), key);
            assert_eq!(action, None, "{key:?} must not start a paper trader");
            assert_eq!(next, ScenarioPrompt::Idle);
        }
    }

    #[test]
    fn stop_is_simple_confirm_and_fires_only_on_y() {
        let t = RunningTrader {
            pid: 4242,
            toml: "/e/2330.toml".to_string(),
            live: true,
        };
        assert!(matches!(
            begin_stop(&t),
            ScenarioPrompt::SimpleStop { pid: 4242, .. }
        ));
        let (next, action) = handle_key(&begin_stop(&t), HaltKey::Char('y'));
        assert_eq!(action, Some(ScenarioAction::Stop { pid: 4242 }));
        assert_eq!(next, ScenarioPrompt::Idle);
        for key in [HaltKey::Char('n'), HaltKey::Enter, HaltKey::Cancel] {
            let (_, action) = handle_key(&begin_stop(&t), key);
            assert_eq!(action, None, "{key:?} must not stop");
        }
    }

    #[test]
    fn idle_ignores_keys() {
        let (next, action) = handle_key(&ScenarioPrompt::Idle, HaltKey::Char('y'));
        assert_eq!(next, ScenarioPrompt::Idle);
        assert_eq!(action, None);
    }

    #[test]
    fn live_argv_has_live_and_yes() {
        let argv = build_spawn_argv(
            "kairos_scenario_trader",
            Path::new("/e/2330.toml"),
            Launch::Live,
        );
        assert_eq!(
            argv,
            vec!["kairos_scenario_trader", "/e/2330.toml", "--live", "--yes"]
        );
    }

    #[test]
    fn paper_argv_has_neither_live_nor_yes() {
        let argv = build_spawn_argv(
            "kairos_scenario_trader",
            Path::new("/e/0050.toml"),
            Launch::Paper,
        );
        assert_eq!(argv, vec!["kairos_scenario_trader", "/e/0050.toml"]);
        assert!(!argv.iter().any(|a| a == "--live"));
        assert!(!argv.iter().any(|a| a == "--yes"));
    }

    #[test]
    fn stop_signal_is_sigint_only_for_a_trader() {
        assert_eq!(stop_signal(true), Some(libc::SIGINT));
        assert_eq!(stop_signal(false), None);
    }

    #[test]
    fn run_stop_fires_exactly_once_when_validated() {
        let calls = std::cell::RefCell::new(Vec::<(i32, i32)>::new());
        let res = run_stop(
            4242,
            |_| true,
            |pid, sig| {
                calls.borrow_mut().push((pid, sig));
                0
            },
        );
        assert!(res.is_ok());
        assert_eq!(*calls.borrow(), vec![(4242, libc::SIGINT)]);
    }

    #[test]
    fn run_stop_never_fires_for_a_non_trader_pid() {
        let calls = std::cell::RefCell::new(Vec::<(i32, i32)>::new());
        let res = run_stop(
            99999,
            |_| false,
            |pid, sig| {
                calls.borrow_mut().push((pid, sig));
                0
            },
        );
        assert!(res.is_err());
        assert!(
            calls.borrow().is_empty(),
            "must not signal an unvalidated pid"
        );
    }

    #[test]
    fn run_stop_surfaces_kill_failure() {
        let res = run_stop(1, |_| true, |_, _| -1);
        assert!(res.is_err());
    }

    #[test]
    fn default_trader_bin_is_derived_from_scenario_dir() {
        let bin = resolve_trader_bin(None, None, Path::new("/home/u/Kairos/exec/scenario"));
        assert_eq!(
            bin,
            PathBuf::from("/home/u/Kairos/exec/scenario/build/kairos_scenario_trader")
        );
    }

    #[test]
    fn default_trader_bin_is_not_a_bare_path_name() {
        // Regression guard for `os error 2`: the default must be a derived,
        // multi-component path, never the bare `kairos_scenario_trader` name that
        // relies on a PATH lookup (which is what made every START fail).
        let bin = resolve_trader_bin(None, None, Path::new("/some/scenario/dir"));
        assert_ne!(bin, PathBuf::from(TRADER_BIN));
        assert!(
            bin.components().count() > 1,
            "default trader-bin must be a derived path, not a bare PATH name"
        );
        assert!(bin.is_absolute() || bin.parent().is_some());
    }

    #[test]
    fn trader_bin_flag_overrides_env_and_default() {
        let bin = resolve_trader_bin(
            Some("/opt/custom/trader"),
            Some("/env/trader"),
            Path::new("/scenario"),
        );
        assert_eq!(bin, PathBuf::from("/opt/custom/trader"));
    }

    #[test]
    fn trader_bin_env_overrides_default_but_not_flag() {
        let bin = resolve_trader_bin(None, Some("/env/trader"), Path::new("/scenario"));
        assert_eq!(bin, PathBuf::from("/env/trader"));
    }

    #[test]
    fn trader_bin_empty_flag_and_env_fall_through_to_default() {
        let bin = resolve_trader_bin(Some(""), Some(""), Path::new("/scenario"));
        assert_eq!(bin, PathBuf::from("/scenario/build/kairos_scenario_trader"));
    }

    #[test]
    fn ensure_trader_bin_missing_yields_clear_error() {
        let dir = std::env::temp_dir().join(format!("kairos-nobin-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let missing = dir.join("build").join(TRADER_BIN);
        let err = ensure_trader_bin(&missing).unwrap_err();
        assert!(err.contains("trader binary not found"), "got: {err}");
        assert!(err.contains(&missing.display().to_string()), "got: {err}");
        assert!(
            err.contains("--trader-bin") && err.contains("KAIROS_SCENARIO_TRADER"),
            "got: {err}"
        );
        assert!(
            !err.contains("os error 2"),
            "must not surface a raw os error: {err}"
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn ensure_trader_bin_accepts_an_executable_file() {
        use std::os::unix::fs::PermissionsExt;
        let dir = std::env::temp_dir().join(format!("kairos-okbin-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let bin = dir.join("kairos_scenario_trader");
        std::fs::write(&bin, b"#!/bin/sh\n").unwrap();
        std::fs::set_permissions(&bin, std::fs::Permissions::from_mode(0o755)).unwrap();
        assert!(ensure_trader_bin(&bin).is_ok());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn merge_matches_running_to_scenario_by_stem() {
        let avail = vec![
            parse_scenario_toml(PathBuf::from("/e/0050.toml"), PAPER_TOML),
            parse_scenario_toml(PathBuf::from("/e/2330.toml"), LIVE_TOML),
        ];
        let running = vec![RunningTrader {
            pid: 4242,
            toml: "/e/2330.toml".to_string(),
            live: true,
        }];
        let rows = merge_rows(&avail, &running);
        assert_eq!(
            rows.len(),
            2,
            "no orphan row: the trader matched a scenario"
        );
        let r2330 = rows.iter().find(|r| r.stem == "2330").unwrap();
        assert!(r2330.is_running());
        assert_eq!(r2330.pid(), Some(4242));
        assert!(r2330.avail.is_some(), "still startable-source once stopped");
        let r0050 = rows.iter().find(|r| r.stem == "0050").unwrap();
        assert!(!r0050.is_running());
        assert_eq!(r0050.pid(), None);
    }

    #[test]
    fn merge_keeps_every_running_pid_as_its_own_row() {
        // Two traders share the "2330" stem (e.g. a daily restart while the old
        // pid is still in SIGINT wind-down). Both must remain stoppable rows.
        let avail = vec![
            parse_scenario_toml(PathBuf::from("/e/2330.toml"), LIVE_TOML),
            parse_scenario_toml(PathBuf::from("/e/0050.toml"), PAPER_TOML),
        ];
        let running = vec![
            RunningTrader {
                pid: 100,
                toml: "/e/2330.toml".to_string(),
                live: true,
            },
            RunningTrader {
                pid: 200,
                toml: "/other/2330.toml".to_string(),
                live: true,
            },
        ];
        let rows = merge_rows(&avail, &running);
        let pids: Vec<i32> = rows.iter().filter_map(|r| r.pid()).collect();
        assert!(
            pids.contains(&100),
            "first same-stem pid must render: {pids:?}"
        );
        assert!(
            pids.contains(&200),
            "second same-stem pid must not be dropped: {pids:?}"
        );
        assert_eq!(
            rows.iter().filter(|r| r.is_running()).count(),
            2,
            "every running trader is its own row"
        );
        // The stopped-only 0050 scenario still renders exactly once.
        let n0050 = rows.iter().filter(|r| r.stem == "0050").count();
        assert_eq!(n0050, 1);
        assert!(
            rows.iter()
                .find(|r| r.stem == "0050")
                .unwrap()
                .pid()
                .is_none()
        );
    }

    #[test]
    fn merge_running_row_count_equals_running_len_for_distinct_pids() {
        let avail = vec![parse_scenario_toml(
            PathBuf::from("/e/2330.toml"),
            LIVE_TOML,
        )];
        let running = vec![
            RunningTrader {
                pid: 1,
                toml: "/e/2330.toml".to_string(),
                live: true,
            },
            RunningTrader {
                pid: 2,
                toml: "/e/ghost.toml".to_string(),
                live: false,
            },
            RunningTrader {
                pid: 3,
                toml: "/e/2330.toml".to_string(),
                live: true,
            },
        ];
        let rows = merge_rows(&avail, &running);
        assert_eq!(
            rows.iter().filter(|r| r.is_running()).count(),
            running.len(),
            "no running trader is dropped"
        );
        let mut pids: Vec<i32> = rows.iter().filter_map(|r| r.pid()).collect();
        pids.sort_unstable();
        assert_eq!(pids, vec![1, 2, 3]);
    }

    #[test]
    fn merge_appends_orphan_trader_not_on_disk() {
        let avail = vec![parse_scenario_toml(
            PathBuf::from("/e/2330.toml"),
            LIVE_TOML,
        )];
        let running = vec![RunningTrader {
            pid: 9001,
            toml: "/e/ghost.toml".to_string(),
            live: false,
        }];
        let rows = merge_rows(&avail, &running);
        assert_eq!(rows.len(), 2);
        let ghost = rows.iter().find(|r| r.stem == "ghost").unwrap();
        assert!(ghost.avail.is_none(), "orphan has no on-disk toml");
        assert!(ghost.is_running());
        assert_eq!(ghost.pid(), Some(9001));
    }

    #[test]
    fn ensure_trader_bin_rejects_a_non_executable_file() {
        use std::os::unix::fs::PermissionsExt;
        let dir = std::env::temp_dir().join(format!("kairos-noexe-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let bin = dir.join("kairos_scenario_trader");
        std::fs::write(&bin, b"not exec\n").unwrap();
        std::fs::set_permissions(&bin, std::fs::Permissions::from_mode(0o644)).unwrap();
        let err = ensure_trader_bin(&bin).unwrap_err();
        assert!(err.contains("not executable"), "got: {err}");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
