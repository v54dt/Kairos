mod feed;
mod journal;
mod recorder;
mod systemd;

use ratatui::Frame;
use ratatui::layout::{Constraint, Direction, Layout};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::Paragraph;

use crate::app::{Config, Snapshot};

const JOURNAL_HEIGHT: u16 = 14;

pub fn render(frame: &mut Frame, snap: &Snapshot, cfg: &Config) {
    let outer = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(1),
            Constraint::Min(0),
            Constraint::Length(JOURNAL_HEIGHT),
        ])
        .split(frame.area());

    let title = Paragraph::new(Line::from(vec![
        Span::styled(
            "kairos-top",
            Style::default()
                .fg(Color::Cyan)
                .add_modifier(Modifier::BOLD),
        ),
        Span::raw("  health panel   "),
        Span::styled("[q] quit", Style::default().fg(Color::DarkGray)),
    ]));
    frame.render_widget(title, outer[0]);

    let mid = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Percentage(50), Constraint::Percentage(50)])
        .split(outer[1]);

    systemd::render(frame, mid[0], &snap.systemd);

    let right = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Percentage(55), Constraint::Percentage(45)])
        .split(mid[1]);
    feed::render(frame, right[0], &snap.feed, cfg);
    recorder::render(frame, right[1], &snap.recorder, snap.disk_free, cfg);

    journal::render(frame, outer[2], &snap.journal);
}

pub(crate) fn error_line(msg: &str) -> Line<'static> {
    Line::from(Span::styled(
        format!("ERROR: {msg}"),
        Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
    ))
}

pub(crate) fn loading_line() -> Line<'static> {
    Line::from(Span::styled(
        "loading...",
        Style::default().fg(Color::DarkGray),
    ))
}

pub(crate) fn human_bytes(bytes: u64) -> String {
    const GIB: u64 = 1 << 30;
    const MIB: u64 = 1 << 20;
    if bytes >= GIB {
        format!("{:.1} GiB", bytes as f64 / GIB as f64)
    } else {
        format!("{:.1} MiB", bytes as f64 / MIB as f64)
    }
}
