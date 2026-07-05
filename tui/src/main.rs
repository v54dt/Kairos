mod app;
mod format;
mod panels;
mod sources;
mod terminal;

use std::io::IsTerminal;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::Result;
use crossterm::event::{Event, EventStream, KeyCode, KeyEventKind, KeyModifiers};
use futures::StreamExt;

use app::{Cli, Config, Shared, Snapshot, Tab};
use sources::feed::{self, FeedState};

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

    tokio::spawn(feed::run(socket, cfg.symbols.clone(), feed_state.clone()));
    tokio::spawn(app::refresh_systemd(shared.clone()));
    tokio::spawn(app::refresh_journal(shared.clone()));
    tokio::spawn(app::refresh_recorder(shared.clone(), cfg.data_dir.clone()));
    tokio::spawn(app::refresh_hub_status(shared.clone()));
    tokio::spawn(app::refresh_scenarios(
        shared.clone(),
        cfg.journal_dir.clone(),
    ));

    let mut term = terminal::enter()?;
    let res = run(&mut term, &shared, &feed_state, &cfg).await;
    terminal::restore()?;
    res
}

async fn run(
    term: &mut terminal::Tui,
    shared: &Shared,
    feed_state: &Mutex<FeedState>,
    cfg: &Config,
) -> Result<()> {
    let mut events = EventStream::new();
    let mut tick = tokio::time::interval(TICK);
    let mut tab = Tab::Overview;
    loop {
        tokio::select! {
            _ = tick.tick() => {
                let snap = Snapshot::capture(shared, feed_state);
                term.draw(|frame| panels::render(frame, &snap, cfg, tab))?;
            }
            Some(Ok(event)) = events.next() => {
                if should_quit(&event) {
                    return Ok(());
                }
                if let Some(next) = tab_for(&event, tab) {
                    tab = next;
                    let snap = Snapshot::capture(shared, feed_state);
                    term.draw(|frame| panels::render(frame, &snap, cfg, tab))?;
                }
            }
        }
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
            KeyCode::Char(c @ ('1' | '2' | '3')) => Some(current.select(c)),
            KeyCode::Tab => Some(current.next()),
            _ => None,
        };
    }
    None
}
