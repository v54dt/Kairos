use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use tokio::time::interval;

use crate::sources::feed::FeedState;
use crate::sources::journald::{self, LogLine};
use crate::sources::recorder::{self, RecorderStats};
use crate::sources::systemd::{self, UnitStatus};

const REFRESH: Duration = Duration::from_secs(2);
const JOURNAL_TAIL: u32 = 10;
const MAX_JOURNAL: usize = 12;
const RECORDER_UNIT: &str = "kairos-recordd.service";
const RECORDER_SCAN: u32 = 200;

#[derive(Clone, Debug, Default)]
pub enum Fetch<T> {
    #[default]
    Loading,
    Ok(T),
    Err(String),
}

#[derive(Clone, Debug)]
pub struct Config {
    pub symbols: Vec<String>,
    pub data_dir: PathBuf,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tab {
    Overview,
    FeedsBooks,
}

impl Tab {
    pub fn select(self, key: char) -> Tab {
        match key {
            '1' => Tab::Overview,
            '2' => Tab::FeedsBooks,
            _ => self,
        }
    }

    pub fn next(self) -> Tab {
        match self {
            Tab::Overview => Tab::FeedsBooks,
            Tab::FeedsBooks => Tab::Overview,
        }
    }
}

pub enum Cli {
    Run(Config),
    Help,
    Version,
}

pub const USAGE: &str = "\
kairos-top - Kairos market-data and health TUI

Usage: kairos-top [OPTIONS]

Options:
  --symbols <A,B,...>   Watchlist symbols (default: 2330,0050)
  --data-dir <PATH>     Recorder data directory
  -h, --help            Print this help and exit
  -V, --version         Print version and exit

Keys: [1] Overview  [2] Feeds & Books  [Tab] switch  [q] quit";

pub fn version_line() -> String {
    format!("kairos-top {}", env!("CARGO_PKG_VERSION"))
}

pub fn parse_args() -> Cli {
    parse_cli(std::env::args().skip(1))
}

pub fn parse_cli<I: IntoIterator<Item = String>>(args: I) -> Cli {
    let mut symbols = vec!["2330".to_string(), "0050".to_string()];
    let mut data_dir = default_data_dir();
    let mut args = args.into_iter();
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--help" | "-h" => return Cli::Help,
            "--version" | "-V" => return Cli::Version,
            "--symbols" => {
                if let Some(v) = args.next() {
                    symbols = v.split(',').map(|s| s.trim().to_string()).collect();
                }
            }
            "--data-dir" => {
                if let Some(v) = args.next() {
                    data_dir = PathBuf::from(v);
                }
            }
            _ => {}
        }
    }
    Cli::Run(Config { symbols, data_dir })
}

fn default_data_dir() -> PathBuf {
    let home = std::env::var("HOME").unwrap_or_else(|_| ".".to_string());
    PathBuf::from(home).join("Kairos").join("data")
}

#[derive(Default)]
pub struct Shared {
    pub systemd: Mutex<Fetch<Vec<UnitStatus>>>,
    pub journal: Mutex<Fetch<Vec<LogLine>>>,
    pub recorder: Mutex<Fetch<Vec<RecorderStats>>>,
    pub disk_free: Mutex<Option<u64>>,
}

pub struct Snapshot {
    pub systemd: Fetch<Vec<UnitStatus>>,
    pub journal: Fetch<Vec<LogLine>>,
    pub recorder: Fetch<Vec<RecorderStats>>,
    pub disk_free: Option<u64>,
    pub feed: FeedState,
}

impl Snapshot {
    pub fn capture(shared: &Shared, feed: &Mutex<FeedState>) -> Self {
        Snapshot {
            systemd: shared.systemd.lock().unwrap().clone(),
            journal: shared.journal.lock().unwrap().clone(),
            recorder: shared.recorder.lock().unwrap().clone(),
            disk_free: *shared.disk_free.lock().unwrap(),
            feed: feed.lock().unwrap().clone(),
        }
    }
}

pub async fn refresh_systemd(shared: Arc<Shared>) {
    let mut tick = interval(REFRESH);
    loop {
        tick.tick().await;
        let r = match systemd::list_units().await {
            Ok(v) => Fetch::Ok(v),
            Err(e) => Fetch::Err(e.to_string()),
        };
        *shared.systemd.lock().unwrap() = r;
    }
}

pub async fn refresh_journal(shared: Arc<Shared>) {
    let mut tick = interval(REFRESH);
    loop {
        tick.tick().await;
        *shared.journal.lock().unwrap() = collect_journal().await;
    }
}

async fn collect_journal() -> Fetch<Vec<LogLine>> {
    let units = match systemd::list_units().await {
        Ok(u) => u,
        Err(e) => return Fetch::Err(e.to_string()),
    };
    let mut lines = Vec::new();
    for u in &units {
        if let Ok(mut ls) = journald::tail_unit(&u.unit, JOURNAL_TAIL).await {
            lines.append(&mut ls);
        }
    }
    lines.sort_by(|a, b| a.ts.cmp(&b.ts));
    let drop = lines.len().saturating_sub(MAX_JOURNAL);
    Fetch::Ok(lines.split_off(drop))
}

pub async fn refresh_recorder(shared: Arc<Shared>, data_dir: PathBuf) {
    let mut tick = interval(REFRESH);
    loop {
        tick.tick().await;
        let r = match recorder::tail_stats(RECORDER_UNIT, RECORDER_SCAN).await {
            Ok(s) => Fetch::Ok(s),
            Err(e) => Fetch::Err(e.to_string()),
        };
        *shared.recorder.lock().unwrap() = r;
        *shared.disk_free.lock().unwrap() = recorder::disk_free_bytes(&data_dir);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(v: &[&str]) -> Vec<String> {
        v.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn help_flag_parses() {
        assert!(matches!(parse_cli(args(&["--help"])), Cli::Help));
        assert!(matches!(parse_cli(args(&["-h"])), Cli::Help));
    }

    #[test]
    fn version_flag_parses() {
        assert!(matches!(parse_cli(args(&["--version"])), Cli::Version));
        assert!(matches!(parse_cli(args(&["-V"])), Cli::Version));
    }

    #[test]
    fn symbols_flag_parses() {
        match parse_cli(args(&["--symbols", "2317, 2454 ,3008"])) {
            Cli::Run(cfg) => assert_eq!(cfg.symbols, vec!["2317", "2454", "3008"]),
            _ => panic!("expected Run"),
        }
    }

    #[test]
    fn defaults_when_absent() {
        match parse_cli(args(&[])) {
            Cli::Run(cfg) => assert_eq!(cfg.symbols, vec!["2330", "0050"]),
            _ => panic!("expected Run"),
        }
    }

    #[test]
    fn data_dir_flag_honored() {
        match parse_cli(args(&["--data-dir", "/tmp/x"])) {
            Cli::Run(cfg) => assert_eq!(cfg.data_dir, PathBuf::from("/tmp/x")),
            _ => panic!("expected Run"),
        }
    }

    #[test]
    fn tab_select_by_key() {
        assert_eq!(Tab::Overview.select('2'), Tab::FeedsBooks);
        assert_eq!(Tab::FeedsBooks.select('1'), Tab::Overview);
    }

    #[test]
    fn tab_select_unknown_key_keeps_state() {
        assert_eq!(Tab::FeedsBooks.select('x'), Tab::FeedsBooks);
        assert_eq!(Tab::Overview.select('9'), Tab::Overview);
    }

    #[test]
    fn tab_next_toggles() {
        assert_eq!(Tab::Overview.next(), Tab::FeedsBooks);
        assert_eq!(Tab::FeedsBooks.next(), Tab::Overview);
    }
}
