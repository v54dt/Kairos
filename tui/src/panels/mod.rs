mod book;
mod confirm_banner;
mod data;
mod feed;
mod fills;
mod journal;
mod listview;
mod recorder;
mod risk;
mod scenarios;
mod systemd;

#[cfg(test)]
pub(crate) mod test_util;

use ratatui::Frame;
use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Clear, Paragraph};

use crate::app::{Config, Snapshot, Tab};
use crate::sources::halt::HaltUi;
use crate::sources::scenario_ctl::ScenarioUi;
use crate::sources::service::ServiceUi;

pub(crate) use journal::DrillView;

const JOURNAL_HEIGHT: u16 = 14;

#[allow(clippy::too_many_arguments)]
pub fn render(
    frame: &mut Frame,
    snap: &Snapshot,
    cfg: &Config,
    tab: Tab,
    halt: &HaltUi,
    service: &ServiceUi,
    scenario: &ScenarioUi,
    fills_sel: usize,
) {
    let outer = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(1), Constraint::Min(0)])
        .split(frame.area());

    frame.render_widget(tab_bar(tab), outer[0]);

    match tab {
        Tab::Overview => render_overview(frame, outer[1], snap, cfg, service),
        Tab::FeedsBooks => book::render(frame, outer[1], &snap.feed, cfg),
        Tab::Scenarios => {
            scenarios::render(frame, outer[1], &snap.scenarios, &snap.supervisor, scenario)
        }
        Tab::Risk => risk::render(frame, outer[1], &snap.scenarios.hub, &snap.blacklist, halt),
        Tab::Data => data::render(frame, outer[1], snap),
        Tab::Fills => fills::render(frame, outer[1], &snap.fills, &snap.fills_date, fills_sel),
    }
}

/// Draw the read-only journal drill-down as a centered overlay. `Clear` wipes
/// the Overview panels underneath so they never bleed through. Returns the
/// scroll position clamped to the rendered content.
pub fn render_drill_overlay(frame: &mut Frame, view: &DrillView) -> usize {
    let area = centered_rect(frame.area(), 90, 80);
    frame.render_widget(Clear, area);
    journal::render_drilldown(frame, area, view)
}

fn centered_rect(area: Rect, pct_x: u16, pct_y: u16) -> Rect {
    let vert = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Percentage((100 - pct_y) / 2),
            Constraint::Percentage(pct_y),
            Constraint::Percentage((100 - pct_y) / 2),
        ])
        .split(area);
    Layout::default()
        .direction(Direction::Horizontal)
        .constraints([
            Constraint::Percentage((100 - pct_x) / 2),
            Constraint::Percentage(pct_x),
            Constraint::Percentage((100 - pct_x) / 2),
        ])
        .split(vert[1])[1]
}

fn tab_bar(tab: Tab) -> Paragraph<'static> {
    let active = Style::default()
        .fg(Color::Black)
        .bg(Color::Cyan)
        .add_modifier(Modifier::BOLD);
    let idle = Style::default().fg(Color::DarkGray);
    Paragraph::new(Line::from(vec![
        Span::styled(
            "kairos-top",
            Style::default()
                .fg(Color::Cyan)
                .add_modifier(Modifier::BOLD),
        ),
        Span::raw("  "),
        Span::styled(
            " 1 Overview ",
            if tab == Tab::Overview { active } else { idle },
        ),
        Span::raw(" "),
        Span::styled(
            " 2 Feeds & Books ",
            if tab == Tab::FeedsBooks { active } else { idle },
        ),
        Span::raw(" "),
        Span::styled(
            " 3 Scenarios ",
            if tab == Tab::Scenarios { active } else { idle },
        ),
        Span::raw(" "),
        Span::styled(" 4 Risk ", if tab == Tab::Risk { active } else { idle }),
        Span::raw(" "),
        Span::styled(
            " 5 Data & Events ",
            if tab == Tab::Data { active } else { idle },
        ),
        Span::raw(" "),
        Span::styled(" 6 Fills ", if tab == Tab::Fills { active } else { idle }),
        Span::raw("   "),
        Span::styled(
            "[Tab] switch  [q] quit",
            Style::default().fg(Color::DarkGray),
        ),
    ]))
}

fn render_overview(
    frame: &mut Frame,
    area: Rect,
    snap: &Snapshot,
    cfg: &Config,
    service: &ServiceUi,
) {
    let rows = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Min(0), Constraint::Length(JOURNAL_HEIGHT)])
        .split(area);

    let mid = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Percentage(50), Constraint::Percentage(50)])
        .split(rows[0]);

    systemd::render(frame, mid[0], &snap.systemd, service);

    let right = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Percentage(55), Constraint::Percentage(45)])
        .split(mid[1]);
    feed::render(frame, right[0], &snap.feed, cfg);
    recorder::render(frame, right[1], &snap.recorder, snap.disk_free, cfg);

    journal::render(frame, rows[1], &snap.journal);
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
