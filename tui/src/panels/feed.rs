use std::time::{Duration, Instant};

use ratatui::Frame;
use ratatui::layout::Rect;
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph};

use crate::app::Config;
use crate::sources::age::format_age;
use crate::sources::feed::FeedState;

const STALE: Duration = Duration::from_secs(30);
const WARN: Duration = Duration::from_secs(5);

fn age_color(age: Duration) -> Color {
    if age >= STALE {
        Color::Red
    } else if age >= WARN {
        Color::Yellow
    } else {
        Color::Green
    }
}

pub fn render(frame: &mut Frame, area: Rect, state: &FeedState, cfg: &Config) {
    let block = Block::default()
        .title(format!("feed [{}]", cfg.symbols.join(",")))
        .borders(Borders::ALL);

    let mut lines: Vec<Line> = Vec::new();
    if state.connected {
        lines.push(Line::from(Span::styled(
            "CONNECTED",
            Style::default()
                .fg(Color::Green)
                .add_modifier(Modifier::BOLD),
        )));
    } else {
        let mut spans = vec![Span::styled(
            "DISCONNECTED",
            Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
        )];
        if let Some(err) = &state.last_error {
            spans.push(Span::styled(
                format!(" {err}"),
                Style::default().fg(Color::DarkGray),
            ));
        }
        lines.push(Line::from(spans));
    }

    if state.per_source.is_empty() {
        lines.push(Line::from(Span::styled(
            "no quotes yet",
            Style::default().fg(Color::DarkGray),
        )));
    } else {
        let now = Instant::now();
        for (source, stat) in &state.per_source {
            let age = now.saturating_duration_since(stat.last_seen);
            let color = age_color(age);
            let mut spans = vec![
                Span::raw(format!("src {source:<5}")),
                Span::raw(format!("{:<8}", stat.last_symbol)),
                Span::styled(
                    format!("age {:<7}", format_age(age)),
                    Style::default().fg(color),
                ),
                Span::styled(
                    format!("n={}", stat.count),
                    Style::default().fg(Color::DarkGray),
                ),
            ];
            if age >= STALE {
                spans.push(Span::styled(
                    "  STALE",
                    Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
                ));
            }
            lines.push(Line::from(spans));
        }
    }

    frame.render_widget(Paragraph::new(lines).block(block), area);
}
