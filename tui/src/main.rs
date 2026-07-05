mod app;
mod format;
mod panels;
mod sources;
mod terminal;

use std::io::IsTerminal;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::Result;
use crossterm::event::{Event, EventStream, KeyCode, KeyEvent, KeyEventKind, KeyModifiers};
use futures::StreamExt;

use app::{Cli, Config, Shared, Snapshot, Tab};
use sources::feed::{self, FeedState};
use sources::halt::{self, HaltAction, HaltKey, HaltPrompt, HaltUi};

const TICK: Duration = Duration::from_millis(750);

#[tokio::main]
async fn main() -> Result<()> {
    let cfg = match app::parse_args() {
        Cli::Run(cfg) => cfg,
        Cli::Help => {
            println!("{}", app::USAGE);
            return Ok(());
        }
        Cli::Version => {
            println!("{}", app::version_line());
            return Ok(());
        }
    };

    if !std::io::stdout().is_terminal() {
        eprintln!("kairos-top: not a terminal; run in an interactive TTY (or use --help).");
        std::process::exit(1);
    }

    let socket = kairos_core::uds::path::quote_socket_path();

    let shared = Arc::new(Shared::default());
    let feed_state = Arc::new(Mutex::new(FeedState::default()));

    let blacklist_path = sources::blacklist::resolve_blacklist_path(
        cfg.blacklist_path.as_ref().and_then(|p| p.to_str()),
        std::env::var("KAIROS_BLACKLIST_CSV").ok().as_deref(),
    );
    let kqr_dir = cfg.data_dir.join("kqr");

    tokio::spawn(feed::run(socket, cfg.symbols.clone(), feed_state.clone()));
    tokio::spawn(app::refresh_systemd(shared.clone()));
    tokio::spawn(app::refresh_journal(shared.clone()));
    tokio::spawn(app::refresh_recorder(shared.clone(), cfg.data_dir.clone()));
    tokio::spawn(app::refresh_hub_status(shared.clone()));
    tokio::spawn(app::refresh_scenarios(
        shared.clone(),
        cfg.journal_dir.clone(),
    ));
    tokio::spawn(app::refresh_timers(shared.clone()));
    tokio::spawn(app::refresh_blacklist(shared.clone(), blacklist_path));
    tokio::spawn(app::refresh_archive(shared.clone(), kqr_dir));
    tokio::spawn(app::refresh_events(shared.clone()));

    let halt_path = halt::hub_halt_path();

    let mut term = terminal::enter()?;
    let res = run(&mut term, &shared, &feed_state, &cfg, halt_path).await;
    terminal::restore()?;
    res
}

async fn run(
    term: &mut terminal::Tui,
    shared: &Shared,
    feed_state: &Mutex<FeedState>,
    cfg: &Config,
    halt_path: Option<PathBuf>,
) -> Result<()> {
    let mut events = EventStream::new();
    let mut tick = tokio::time::interval(TICK);
    let mut tab = Tab::Overview;
    let mut prompt = HaltPrompt::Idle;
    let mut halt_result: Option<String> = None;
    loop {
        tokio::select! {
            _ = tick.tick() => {
                let snap = Snapshot::capture(shared, feed_state);
                let ui = halt_ui(&halt_path, &prompt, &halt_result);
                term.draw(|frame| panels::render(frame, &snap, cfg, tab, &ui))?;
            }
            Some(Ok(event)) = events.next() => {
                let key = match &event {
                    Event::Key(k) if k.kind == KeyEventKind::Press => *k,
                    _ => continue,
                };
                let mut redraw = false;
                if prompt.is_active() {
                    if let Some(hk) = to_halt_key(&key) {
                        let (next, action) = halt::handle_key(&prompt, hk);
                        prompt = next;
                        if let Some(a) = action {
                            halt_result = Some(apply_halt(a, halt_path.as_deref()));
                        }
                        redraw = true;
                    }
                } else if tab == Tab::Risk
                    && matches!(key.code, KeyCode::Char('k') | KeyCode::Char('c'))
                {
                    if halt_path.is_some() {
                        prompt = match key.code {
                            KeyCode::Char('k') => HaltPrompt::ConfirmHalt(String::new()),
                            _ => HaltPrompt::ConfirmResume(String::new()),
                        };
                    } else {
                        halt_result = Some("kill switch unavailable (no runtime dir)".to_string());
                    }
                    redraw = true;
                } else if should_quit(&event) {
                    return Ok(());
                } else if let Some(next) = tab_for(&event, tab) {
                    tab = next;
                    redraw = true;
                }
                if redraw {
                    let snap = Snapshot::capture(shared, feed_state);
                    let ui = halt_ui(&halt_path, &prompt, &halt_result);
                    term.draw(|frame| panels::render(frame, &snap, cfg, tab, &ui))?;
                }
            }
        }
    }
}

fn halt_ui(path: &Option<PathBuf>, prompt: &HaltPrompt, last_result: &Option<String>) -> HaltUi {
    HaltUi {
        path: path.clone(),
        prompt: prompt.clone(),
        last_result: last_result.clone(),
    }
}

fn to_halt_key(key: &KeyEvent) -> Option<HaltKey> {
    match key.code {
        KeyCode::Char(c) => Some(HaltKey::Char(c)),
        KeyCode::Enter => Some(HaltKey::Enter),
        KeyCode::Backspace => Some(HaltKey::Backspace),
        KeyCode::Esc => Some(HaltKey::Cancel),
        _ => None,
    }
}

fn apply_halt(action: HaltAction, path: Option<&Path>) -> String {
    let path = match path {
        Some(p) => p,
        None => return "kill switch unavailable (no runtime dir)".to_string(),
    };
    match action {
        HaltAction::Arm => match halt::arm_halt(path) {
            Ok(()) => "adminHalt armed".to_string(),
            Err(e) => format!("arm failed: {e}"),
        },
        HaltAction::Clear => match halt::clear_halt(path) {
            Ok(()) => "halt cleared".to_string(),
            Err(e) => format!("clear failed: {e}"),
        },
    }
}

fn should_quit(event: &Event) -> bool {
    if let Event::Key(key) = event {
        if key.kind != KeyEventKind::Press {
            return false;
        }
        return matches!(key.code, KeyCode::Char('q') | KeyCode::Char('Q'))
            || (key.modifiers.contains(KeyModifiers::CONTROL)
                && matches!(key.code, KeyCode::Char('c')));
    }
    false
}

fn tab_for(event: &Event, current: Tab) -> Option<Tab> {
    if let Event::Key(key) = event {
        if key.kind != KeyEventKind::Press {
            return None;
        }
        return match key.code {
            KeyCode::Char(c @ ('1' | '2' | '3' | '4' | '5')) => Some(current.select(c)),
            KeyCode::Tab => Some(current.next()),
            _ => None,
        };
    }
    None
}
