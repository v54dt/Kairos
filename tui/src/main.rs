mod app;
mod panels;
mod sources;
mod terminal;

use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::Result;
use crossterm::event::{Event, EventStream, KeyCode, KeyEventKind, KeyModifiers};
use futures::StreamExt;

use app::{Config, Shared, Snapshot};
use sources::feed::{self, FeedState};

const TICK: Duration = Duration::from_millis(750);

#[tokio::main]
async fn main() -> Result<()> {
    let cfg = app::parse_args();
    let socket = kairos_core::uds::path::quote_socket_path();

    let shared = Arc::new(Shared::default());
    let feed_state = Arc::new(Mutex::new(FeedState::default()));

    tokio::spawn(feed::run(socket, cfg.symbols.clone(), feed_state.clone()));
    tokio::spawn(app::refresh_systemd(shared.clone()));
    tokio::spawn(app::refresh_journal(shared.clone()));
    tokio::spawn(app::refresh_recorder(shared.clone(), cfg.data_dir.clone()));

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
    loop {
        tokio::select! {
            _ = tick.tick() => {
                let snap = Snapshot::capture(shared, feed_state);
                term.draw(|frame| panels::render(frame, &snap, cfg))?;
            }
            Some(Ok(event)) = events.next() => {
                if should_quit(&event) {
                    return Ok(());
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
