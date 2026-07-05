use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use tokio::time::interval;

use std::time::SystemTime;

use crate::sources::archive::{self, ArchiveScan, ShipVerify};
use crate::sources::blacklist::{self, BlacklistFreshness};
use crate::sources::feed::FeedState;
use crate::sources::hub_status::{self, HubReport};
use crate::sources::journald::{self, LogLine};
use crate::sources::order_journal::{self, ScenarioJournal, ScenariosView};
use crate::sources::recorder::{self, RecorderStats};
use crate::sources::scenario_ctl::{self, RunningTrader, ScenarioToml};
use crate::sources::systemd::{self, UnitStatus};
use crate::sources::timers::{self, TimerEntry};

const REFRESH: Duration = Duration::from_secs(2);
const JOURNAL_TAIL: u32 = 10;
pub const DRILLDOWN_TAIL: u32 = 200;
const MAX_JOURNAL: usize = 12;
const EVENTS_TAIL: u32 = 50;
const MAX_EVENTS: usize = 50;
const RECORDER_UNIT: &str = "kairos-recordd.service";
const RECORDER_SCAN: u32 = 200;
const SHIP_UNIT: &str = "kairos-record-ship.service";
const SHIP_SCAN: u32 = 200;

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
    pub journal_dir: PathBuf,
    pub blacklist_path: Option<PathBuf>,
    pub scenario_dir: PathBuf,
    pub trader_bin: PathBuf,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tab {
    Overview,
    FeedsBooks,
    Scenarios,
    Risk,
    Data,
}

impl Tab {
    pub fn select(self, key: char) -> Tab {
        match key {
            '1' => Tab::Overview,
            '2' => Tab::FeedsBooks,
            '3' => Tab::Scenarios,
            '4' => Tab::Risk,
            '5' => Tab::Data,
            _ => self,
        }
    }

    pub fn next(self) -> Tab {
        match self {
            Tab::Overview => Tab::FeedsBooks,
            Tab::FeedsBooks => Tab::Scenarios,
            Tab::Scenarios => Tab::Risk,
            Tab::Risk => Tab::Data,
            Tab::Data => Tab::Overview,
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
  --symbols <A,B,...>    Watchlist symbols (default: 2330,0050)
  --data-dir <PATH>      Recorder data directory
  --journal-dir <PATH>   Order-journal directory (default: <data-dir>/journal)
  --blacklist-path <PATH> Restricted-symbol blacklist CSV (F1 gate)
  --scenario-dir <PATH>  Scenario .toml directory (default: ~/Kairos/exec/scenario)
  --trader-bin <PATH>    Scenario-trader executable (default: <scenario-dir>/build/kairos_scenario_trader)
  -h, --help             Print this help and exit
  -V, --version          Print version and exit

Keys: [1] Overview  [2] Feeds & Books  [3] Scenarios  [4] Risk  [5] Data & Events  [Tab] switch  [q] quit
Overview tab: [up/down] select unit  [Enter] open its journal  [r]estart [s]tart [x]stop [f] reset-failed  [S]/[X] kairos.target up/down
              trading units require a typed confirm (the unit name); research crons and reset-failed are y/N
              in the journal view: [up/down][PgUp/PgDn] scroll (newest at bottom)  [Esc] close
Scenarios tab: [left/right] focus Available/Running  [up/down] select  [s]tart selected  [x]stop selected
               starting a LIVE toml needs a typed confirm (the toml stem); a PAPER start and any stop are y/N
               the trader binary defaults to <scenario-dir>/build/kairos_scenario_trader (--trader-bin or KAIROS_SCENARIO_TRADER override)
Risk tab: [k] arm adminHalt (type HALT)  [c] clear halt (type RESUME)";

pub fn version_line() -> String {
    format!("kairos-top {}", env!("CARGO_PKG_VERSION"))
}

pub fn parse_args() -> Cli {
    parse_cli(std::env::args().skip(1))
}

pub fn parse_cli<I: IntoIterator<Item = String>>(args: I) -> Cli {
    let mut symbols = vec!["2330".to_string(), "0050".to_string()];
    let mut data_dir = default_data_dir();
    let mut journal_dir: Option<PathBuf> = None;
    let mut blacklist_path: Option<PathBuf> = None;
    let mut scenario_dir: Option<PathBuf> = None;
    let mut trader_bin_flag: Option<String> = None;
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
            "--journal-dir" => {
                if let Some(v) = args.next() {
                    journal_dir = Some(PathBuf::from(v));
                }
            }
            "--blacklist-path" => {
                if let Some(v) = args.next() {
                    blacklist_path = Some(PathBuf::from(v));
                }
            }
            "--scenario-dir" => {
                if let Some(v) = args.next() {
                    scenario_dir = Some(PathBuf::from(v));
                }
            }
            "--trader-bin" => {
                if let Some(v) = args.next() {
                    trader_bin_flag = Some(v);
                }
            }
            _ => {}
        }
    }
    let journal_dir = journal_dir.unwrap_or_else(|| data_dir.join("journal"));
    let scenario_dir = scenario_dir.unwrap_or_else(default_scenario_dir);
    let trader_bin = scenario_ctl::resolve_trader_bin(
        trader_bin_flag.as_deref(),
        std::env::var("KAIROS_SCENARIO_TRADER").ok().as_deref(),
        &scenario_dir,
    );
    Cli::Run(Config {
        symbols,
        data_dir,
        journal_dir,
        blacklist_path,
        scenario_dir,
        trader_bin,
    })
}

fn default_data_dir() -> PathBuf {
    let home = std::env::var("HOME").unwrap_or_else(|_| ".".to_string());
    PathBuf::from(home).join("Kairos").join("data")
}

fn default_scenario_dir() -> PathBuf {
    let home = std::env::var("HOME").unwrap_or_else(|_| ".".to_string());
    PathBuf::from(home)
        .join("Kairos")
        .join("exec")
        .join("scenario")
}

#[derive(Default)]
pub struct Shared {
    pub systemd: Mutex<Fetch<Vec<UnitStatus>>>,
    pub journal: Mutex<Fetch<Vec<LogLine>>>,
    pub recorder: Mutex<Fetch<Vec<RecorderStats>>>,
    pub disk_free: Mutex<Option<u64>>,
    pub hub: Mutex<Option<HubReport>>,
    pub scenarios: Mutex<Vec<ScenarioJournal>>,
    pub available: Mutex<(Vec<ScenarioToml>, usize)>,
    pub running: Mutex<Vec<RunningTrader>>,
    pub timers: Mutex<Fetch<Vec<TimerEntry>>>,
    pub blacklist: Mutex<Fetch<BlacklistFreshness>>,
    pub archive: Mutex<Fetch<ArchiveScan>>,
    pub ship_verify: Mutex<Fetch<Option<ShipVerify>>>,
    pub events: Mutex<Fetch<Vec<LogLine>>>,
}

pub struct Snapshot {
    pub systemd: Fetch<Vec<UnitStatus>>,
    pub journal: Fetch<Vec<LogLine>>,
    pub recorder: Fetch<Vec<RecorderStats>>,
    pub disk_free: Option<u64>,
    pub feed: FeedState,
    pub scenarios: ScenariosView,
    pub available: (Vec<ScenarioToml>, usize),
    pub running: Vec<RunningTrader>,
    pub timers: Fetch<Vec<TimerEntry>>,
    pub blacklist: Fetch<BlacklistFreshness>,
    pub archive: Fetch<ArchiveScan>,
    pub ship_verify: Fetch<Option<ShipVerify>>,
    pub events: Fetch<Vec<LogLine>>,
}

impl Snapshot {
    pub fn capture(shared: &Shared, feed: &Mutex<FeedState>) -> Self {
        let scenarios = order_journal::merge(
            shared.scenarios.lock().unwrap().clone(),
            shared.hub.lock().unwrap().clone(),
        );
        Snapshot {
            systemd: shared.systemd.lock().unwrap().clone(),
            journal: shared.journal.lock().unwrap().clone(),
            recorder: shared.recorder.lock().unwrap().clone(),
            disk_free: *shared.disk_free.lock().unwrap(),
            feed: feed.lock().unwrap().clone(),
            scenarios,
            available: shared.available.lock().unwrap().clone(),
            running: shared.running.lock().unwrap().clone(),
            timers: shared.timers.lock().unwrap().clone(),
            blacklist: shared.blacklist.lock().unwrap().clone(),
            archive: shared.archive.lock().unwrap().clone(),
            ship_verify: shared.ship_verify.lock().unwrap().clone(),
            events: shared.events.lock().unwrap().clone(),
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
        *shared.journal.lock().unwrap() = collect_journal(MAX_JOURNAL, JOURNAL_TAIL).await;
    }
}

async fn collect_journal(max: usize, per_unit: u32) -> Fetch<Vec<LogLine>> {
    let units = match systemd::list_units().await {
        Ok(u) => u,
        Err(e) => return Fetch::Err(e.to_string()),
    };
    let mut lines = Vec::new();
    for u in &units {
        if let Ok(mut ls) = journald::tail_unit(&u.unit, per_unit).await {
            lines.append(&mut ls);
        }
    }
    lines.sort_by(|a, b| a.ts.cmp(&b.ts));
    let drop = lines.len().saturating_sub(max);
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

pub async fn refresh_hub_status(shared: Arc<Shared>) {
    let mut tick = interval(REFRESH);
    loop {
        tick.tick().await;
        let report = hub_status::hub_status_path().and_then(|p| hub_status::read_hub_status(&p));
        *shared.hub.lock().unwrap() = report;
    }
}

pub async fn refresh_scenarios(shared: Arc<Shared>, journal_dir: PathBuf) {
    let mut tick = interval(REFRESH);
    loop {
        tick.tick().await;
        let date = order_journal::today_tw();
        *shared.scenarios.lock().unwrap() = order_journal::scan_journal(&journal_dir, &date);
    }
}

pub async fn refresh_available(shared: Arc<Shared>, scenario_dir: PathBuf) {
    let mut tick = interval(REFRESH);
    loop {
        tick.tick().await;
        *shared.available.lock().unwrap() = scenario_ctl::enumerate_available(&scenario_dir);
    }
}

pub async fn refresh_running(shared: Arc<Shared>) {
    let mut tick = interval(REFRESH);
    loop {
        tick.tick().await;
        *shared.running.lock().unwrap() = scenario_ctl::enumerate_running();
    }
}

pub async fn refresh_timers(shared: Arc<Shared>) {
    let mut tick = interval(REFRESH);
    loop {
        tick.tick().await;
        let r = match timers::collect_timers().await {
            Ok(v) => Fetch::Ok(v),
            Err(e) => Fetch::Err(e.to_string()),
        };
        *shared.timers.lock().unwrap() = r;
    }
}

pub async fn refresh_blacklist(shared: Arc<Shared>, path: PathBuf) {
    let mut tick = interval(REFRESH);
    loop {
        tick.tick().await;
        let f = blacklist::read_blacklist(&path, SystemTime::now());
        *shared.blacklist.lock().unwrap() = Fetch::Ok(f);
    }
}

pub async fn refresh_archive(shared: Arc<Shared>, kqr_dir: PathBuf) {
    let mut tick = interval(REFRESH);
    loop {
        tick.tick().await;
        let today = order_journal::today_tw();
        let yesterday = order_journal::yesterday_tw();
        let scan = archive::scan_archive(&kqr_dir, &today, &yesterday);
        *shared.archive.lock().unwrap() = Fetch::Ok(scan);
        let sv = match archive::ship_verify(SHIP_UNIT, SHIP_SCAN).await {
            Ok(v) => Fetch::Ok(v),
            Err(e) => Fetch::Err(e.to_string()),
        };
        *shared.ship_verify.lock().unwrap() = sv;
    }
}

pub async fn refresh_events(shared: Arc<Shared>) {
    let mut tick = interval(REFRESH);
    loop {
        tick.tick().await;
        *shared.events.lock().unwrap() = collect_journal(MAX_EVENTS, EVENTS_TAIL).await;
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
        assert_eq!(Tab::Overview.select('3'), Tab::Scenarios);
        assert_eq!(Tab::Scenarios.select('2'), Tab::FeedsBooks);
        assert_eq!(Tab::Overview.select('4'), Tab::Risk);
        assert_eq!(Tab::Overview.select('5'), Tab::Data);
        assert_eq!(Tab::Data.select('1'), Tab::Overview);
    }

    #[test]
    fn tab_select_unknown_key_keeps_state() {
        assert_eq!(Tab::FeedsBooks.select('x'), Tab::FeedsBooks);
        assert_eq!(Tab::Overview.select('9'), Tab::Overview);
        assert_eq!(Tab::Scenarios.select('z'), Tab::Scenarios);
        assert_eq!(Tab::Risk.select('z'), Tab::Risk);
        assert_eq!(Tab::Data.select('z'), Tab::Data);
    }

    #[test]
    fn tab_next_cycles() {
        assert_eq!(Tab::Overview.next(), Tab::FeedsBooks);
        assert_eq!(Tab::FeedsBooks.next(), Tab::Scenarios);
        assert_eq!(Tab::Scenarios.next(), Tab::Risk);
        assert_eq!(Tab::Risk.next(), Tab::Data);
        assert_eq!(Tab::Data.next(), Tab::Overview);
    }

    #[test]
    fn blacklist_path_flag_honored() {
        match parse_cli(args(&["--blacklist-path", "/tmp/bl.csv"])) {
            Cli::Run(cfg) => assert_eq!(cfg.blacklist_path, Some(PathBuf::from("/tmp/bl.csv"))),
            _ => panic!("expected Run"),
        }
    }

    #[test]
    fn blacklist_path_defaults_none() {
        match parse_cli(args(&[])) {
            Cli::Run(cfg) => assert_eq!(cfg.blacklist_path, None),
            _ => panic!("expected Run"),
        }
    }

    #[test]
    fn journal_dir_defaults_under_data_dir() {
        match parse_cli(args(&["--data-dir", "/tmp/x"])) {
            Cli::Run(cfg) => assert_eq!(cfg.journal_dir, PathBuf::from("/tmp/x/journal")),
            _ => panic!("expected Run"),
        }
    }

    #[test]
    fn journal_dir_flag_honored() {
        match parse_cli(args(&["--journal-dir", "/tmp/j"])) {
            Cli::Run(cfg) => assert_eq!(cfg.journal_dir, PathBuf::from("/tmp/j")),
            _ => panic!("expected Run"),
        }
    }

    #[test]
    fn scenario_dir_flag_honored() {
        match parse_cli(args(&["--scenario-dir", "/tmp/s"])) {
            Cli::Run(cfg) => assert_eq!(cfg.scenario_dir, PathBuf::from("/tmp/s")),
            _ => panic!("expected Run"),
        }
    }

    #[test]
    fn scenario_dir_defaults_under_home() {
        match parse_cli(args(&[])) {
            Cli::Run(cfg) => assert!(cfg.scenario_dir.ends_with("exec/scenario")),
            _ => panic!("expected Run"),
        }
    }

    #[test]
    fn trader_bin_flag_honored() {
        match parse_cli(args(&["--trader-bin", "/opt/kairos_scenario_trader"])) {
            Cli::Run(cfg) => {
                assert_eq!(cfg.trader_bin, PathBuf::from("/opt/kairos_scenario_trader"))
            }
            _ => panic!("expected Run"),
        }
    }

    #[test]
    fn trader_bin_defaults_under_scenario_dir_build() {
        match parse_cli(args(&["--scenario-dir", "/tmp/s"])) {
            Cli::Run(cfg) => assert_eq!(
                cfg.trader_bin,
                PathBuf::from("/tmp/s/build/kairos_scenario_trader")
            ),
            _ => panic!("expected Run"),
        }
    }
}
