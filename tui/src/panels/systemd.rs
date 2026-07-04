use ratatui::Frame;
use ratatui::layout::Rect;
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph};

use crate::app::Fetch;
use crate::panels::{error_line, loading_line};
use crate::sources::systemd::UnitStatus;

fn active_color(active: &str) -> Color {
    match active {
        "active" => Color::Green,
        "failed" => Color::Red,
        "activating" | "deactivating" | "reloading" => Color::Yellow,
        "inactive" => Color::DarkGray,
        _ => Color::White,
    }
}

fn unit_line(u: &UnitStatus) -> Line<'static> {
    let color = active_color(&u.active);
    Line::from(vec![
        Span::raw(format!("{:<28}", u.unit)),
        Span::styled(format!("{:<11}", u.active), Style::default().fg(color)),
        Span::styled(
            u.sub.clone(),
            Style::default().fg(color).add_modifier(Modifier::DIM),
        ),
    ])
}

pub fn render(frame: &mut Frame, area: Rect, state: &Fetch<Vec<UnitStatus>>) {
    let block = Block::default()
        .title("systemd (kairos-*)")
        .borders(Borders::ALL);
    let lines: Vec<Line> = match state {
        Fetch::Loading => vec![loading_line()],
        Fetch::Err(e) => vec![error_line(e)],
        Fetch::Ok(units) if units.is_empty() => {
            vec![Line::from(Span::styled(
                "no kairos-* units",
                Style::default().fg(Color::DarkGray),
            ))]
        }
        Fetch::Ok(units) => units.iter().map(unit_line).collect(),
    };
    frame.render_widget(Paragraph::new(lines).block(block), area);
}
