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

pub fn parse_args() -> Config {
    let mut symbols = vec!["2330".to_string(), "0050".to_string()];
    let mut data_dir = default_data_dir();
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
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
    Config { symbols, data_dir }
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
