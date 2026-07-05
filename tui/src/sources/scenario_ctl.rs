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
}
