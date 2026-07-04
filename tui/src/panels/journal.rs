use ratatui::Frame;
use ratatui::layout::Rect;
use ratatui::style::{Color, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph};

use crate::app::Fetch;
use crate::panels::{error_line, loading_line};
use crate::sources::journald::LogLine;

fn short_unit(unit: &str) -> &str {
    unit.strip_suffix(".service").unwrap_or(unit)
}

fn log_line(l: &LogLine) -> Line<'static> {
    Line::from(vec![
        Span::styled(format!("{} ", l.ts), Style::default().fg(Color::DarkGray)),
        Span::styled(
            format!("{:<18} ", short_unit(&l.unit)),
            Style::default().fg(Color::Cyan),
        ),
        Span::raw(l.message.clone()),
    ])
}

pub fn render(frame: &mut Frame, area: Rect, state: &Fetch<Vec<LogLine>>) {
    let block = Block::default().title("events").borders(Borders::ALL);
    let lines: Vec<Line> = match state {
        Fetch::Loading => vec![loading_line()],
        Fetch::Err(e) => vec![error_line(e)],
        Fetch::Ok(logs) if logs.is_empty() => vec![Line::from(Span::styled(
            "no events",
            Style::default().fg(Color::DarkGray),
        ))],
        Fetch::Ok(logs) => logs.iter().map(log_line).collect(),
    };
    frame.render_widget(Paragraph::new(lines).block(block), area);
}
