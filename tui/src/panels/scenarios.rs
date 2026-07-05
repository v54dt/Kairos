use std::time::{Duration, SystemTime, UNIX_EPOCH};

use ratatui::Frame;
use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph};

use crate::sources::age::format_age;
use crate::sources::hub_status::HubReport;
use crate::sources::order_journal::{ScenarioJournal, ScenariosView};

const HUB_HEIGHT: u16 = 9;

fn now_us() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_micros() as i64)
        .unwrap_or(0)
}

fn dim(text: &str) -> Line<'static> {
    Line::from(Span::styled(
        text.to_string(),
        Style::default().fg(Color::DarkGray),
    ))
}

fn age_from_us(now: i64, event_us: i64) -> String {
    if event_us <= 0 || now <= event_us {
        return "-".to_string();
    }
    format_age(Duration::from_micros((now - event_us) as u64))
}

fn scenario_lines(scenarios: &[ScenarioJournal], now: i64) -> Vec<Line<'static>> {
    let mut lines = vec![Line::from(Span::styled(
        format!(
            "{:<22} {:>6} {:>10} {:>14} {:>6} {:>7}",
            "scenario", "fills", "shares", "NT$", "cxl", "last"
        ),
        Style::default().fg(Color::DarkGray),
    ))];
    if scenarios.is_empty() {
        lines.push(dim("no journal files for today"));
        return lines;
    }
    for s in scenarios {
        let ntd = s.filled_notional_cents / 100;
        lines.push(Line::from(format!(
            "{:<22} {:>6} {:>10} {:>14} {:>6} {:>7}",
            s.name,
            s.fills,
            s.filled_shares,
            ntd,
            s.cancels,
            age_from_us(now, s.last_event_us),
        )));
    }
    lines
}

fn hub_lines(hub: &Option<HubReport>, now_s: i64) -> Vec<Line<'static>> {
    let report = match hub {
        None => {
            return vec![Line::from(Span::styled(
                "hub offline",
                Style::default()
                    .fg(Color::DarkGray)
                    .add_modifier(Modifier::BOLD),
            ))];
        }
        Some(r) => r,
    };

    let mut header = vec![
        Span::styled(
            format!("clients {}", report.status.client_count),
            Style::default().add_modifier(Modifier::BOLD),
        ),
        Span::raw("  "),
    ];
    if report.is_stale() {
        header.push(Span::styled(
            format!("STALE {}", format_age(report.age)),
            Style::default()
                .fg(Color::Yellow)
                .add_modifier(Modifier::BOLD),
        ));
    } else {
        header.push(Span::styled(
            format!("age {}", format_age(report.age)),
            Style::default().fg(Color::Green),
        ));
    }
    let mut lines = vec![Line::from(header)];

    lines.push(Line::from(Span::styled(
        format!(
            "{:<10} {:>6} {:>5} {:>5} {:>5} {:>5} {:>7}",
            "client", "pid", "open", "sub", "fill", "cxl", "idle"
        ),
        Style::default().fg(Color::DarkGray),
    )));
    if report.status.clients.is_empty() {
        lines.push(dim("no connected clients"));
    }
    for c in &report.status.clients {
        let idle = if c.last_activity_s > 0 && now_s > c.last_activity_s {
            format_age(Duration::from_secs((now_s - c.last_activity_s) as u64))
        } else {
            "-".to_string()
        };
        let open_style = if c.open > 0 {
            Style::default().fg(Color::Cyan)
        } else {
            Style::default()
        };
        lines.push(Line::from(vec![Span::styled(
            format!(
                "{:<10} {:>6} {:>5} {:>5} {:>5} {:>5} {:>7}",
                c.prefix, c.pid, c.open, c.submitted, c.filled, c.cancelled, idle
            ),
            open_style,
        )]));
    }
    lines
}

pub fn render(frame: &mut Frame, area: Rect, view: &ScenariosView) {
    let rows = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Min(0), Constraint::Length(HUB_HEIGHT)])
        .split(area);

    let now = now_us();
    frame.render_widget(
        Paragraph::new(scenario_lines(&view.scenarios, now)).block(
            Block::default()
                .title("scenarios (today)")
                .borders(Borders::ALL),
        ),
        rows[0],
    );
    frame.render_widget(
        Paragraph::new(hub_lines(&view.hub, now / 1_000_000))
            .block(Block::default().title("order hub").borders(Borders::ALL)),
        rows[1],
    );
}
