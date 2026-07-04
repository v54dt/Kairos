mod terminal;

use std::time::Duration;

use anyhow::Result;
use crossterm::event::{Event, EventStream, KeyCode, KeyEventKind, KeyModifiers};
use futures::StreamExt;
use ratatui::layout::Alignment;
use ratatui::widgets::{Block, Borders, Paragraph};

const TICK: Duration = Duration::from_millis(750);

#[tokio::main]
async fn main() -> Result<()> {
    let mut term = terminal::enter()?;
    let res = run(&mut term).await;
    terminal::restore()?;
    res
}

async fn run(term: &mut terminal::Tui) -> Result<()> {
    let mut events = EventStream::new();
    let mut tick = tokio::time::interval(TICK);
    loop {
        tokio::select! {
            _ = tick.tick() => {
                term.draw(|frame| {
                    let block = Block::default()
                        .title("kairos-top")
                        .borders(Borders::ALL);
                    let body = Paragraph::new("health panel")
                        .block(block)
                        .alignment(Alignment::Left);
                    frame.render_widget(body, frame.area());
                })?;
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
