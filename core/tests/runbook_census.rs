//! Env-census honesty test: every runtime `KAIROS_*` variable that appears in the
//! shipped source must be documented in `docs/RUNBOOK.md`. Fails when someone adds
//! a new var without documenting it. Scans SOURCE dirs only (not tests/build), and
//! filters C++ include-guard tokens (`..._H_`, `KAIROS_EXEC_...`) and partial
//! prefixes (a trailing `_`, e.g. a `KAIROS_SIM_*` doc comment).

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

fn repo_root() -> PathBuf {
    // CARGO_MANIFEST_DIR is <repo>/core.
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("core has a parent")
        .to_path_buf()
}

fn source_dirs(root: &Path) -> Vec<PathBuf> {
    [
        "core/src",
        "exec/scenario/src",
        "tui/src",
        "sidecar/concords/src",
    ]
    .iter()
    .map(|d| root.join(d))
    .collect()
}

fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            walk(&path, out);
        } else {
            out.push(path);
        }
    }
}

/// Extract `KAIROS_[A-Z0-9_]+` tokens from `text`.
fn extract(text: &str, into: &mut BTreeSet<String>) {
    let bytes = text.as_bytes();
    const PREFIX: &[u8] = b"KAIROS_";
    let mut i = 0;
    while i + PREFIX.len() <= bytes.len() {
        if &bytes[i..i + PREFIX.len()] == PREFIX {
            let mut j = i + PREFIX.len();
            while j < bytes.len()
                && (bytes[j].is_ascii_uppercase() || bytes[j].is_ascii_digit() || bytes[j] == b'_')
            {
                j += 1;
            }
            into.insert(String::from_utf8_lossy(&bytes[i..j]).into_owned());
            i = j;
        } else {
            i += 1;
        }
    }
}

/// A runtime env var (not an include guard or a `KAIROS_SIM_*`-style partial).
fn is_runtime_var(tok: &str) -> bool {
    !tok.ends_with("_H_") && !tok.contains("_EXEC_") && !tok.ends_with('_')
}

#[test]
fn every_source_kairos_var_is_documented() {
    let root = repo_root();

    let mut tokens = BTreeSet::new();
    for dir in source_dirs(&root) {
        let mut files = Vec::new();
        walk(&dir, &mut files);
        for f in files {
            if let Ok(text) = std::fs::read_to_string(&f) {
                extract(&text, &mut tokens);
            }
        }
    }
    let vars: Vec<String> = tokens.into_iter().filter(|t| is_runtime_var(t)).collect();
    assert!(
        !vars.is_empty(),
        "census found no KAIROS_* vars — the scan is broken, not the docs"
    );

    let runbook_path = root.join("docs/RUNBOOK.md");
    let runbook = std::fs::read_to_string(&runbook_path)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", runbook_path.display()));

    let missing: Vec<&String> = vars
        .iter()
        .filter(|v| !runbook.contains(v.as_str()))
        .collect();
    assert!(
        missing.is_empty(),
        "these KAIROS_* vars are used in source but undocumented in docs/RUNBOOK.md: {missing:?}"
    );
}
