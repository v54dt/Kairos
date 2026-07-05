use std::path::{Path, PathBuf};

/// One scenario `.toml` discovered on disk, parsed just enough to drive the
/// start UI: its name/symbol for display and whether it launches LIVE.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ScenarioToml {
    pub path: PathBuf,
    pub name: String,
    pub symbol: String,
    pub live: bool,
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
/// templates and non-`.toml` files; an unreadable toml is counted in the
/// returned `skipped` total and left out rather than crashing the scan.
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
            Ok(text) => out.push(parse_scenario_toml(entry.path(), &text)),
            Err(_) => skipped += 1,
        }
    }
    out.sort_by(|a, b| a.path.cmp(&b.path));
    (out, skipped)
}

/// The scenario-trader binary name (a PATH lookup). Override for tests/installs
/// with `$KAIROS_SCENARIO_TRADER`.
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
        // An unreadable toml: a directory named like one.
        std::fs::create_dir_all(dir.join("broken.toml")).unwrap();

        let (found, skipped) = enumerate_available(&dir);
        let names: Vec<_> = found.iter().map(|s| s.symbol.clone()).collect();
        assert!(names.contains(&"2330".to_string()));
        assert!(names.contains(&"0050".to_string()));
        assert_eq!(found.len(), 2, "example + txt excluded, broken skipped");
        assert_eq!(skipped, 1);

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
}
