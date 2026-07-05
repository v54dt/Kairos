use ratatui::Frame;
use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Gauge, Paragraph};

use crate::app::Fetch;
use crate::format::format_price;
use crate::sources::age::format_age;
use crate::sources::blacklist::BlacklistFreshness;
use crate::sources::hub_status::HubReport;

const MAX_BLACKLIST_ROWS: usize = 8;

fn ntd(cents: i64) -> String {
    format!("NT${}", format_price(cents, 2))
}

fn halt_banner(hub: &Option<HubReport>) -> Paragraph<'static> {
    let (text, style) = match hub {
        None => (
            "  HUB OFFLINE - halt state unknown  ".to_string(),
            Style::default()
                .fg(Color::Black)
                .bg(Color::DarkGray)
                .add_modifier(Modifier::BOLD),
        ),
        Some(r) if r.status.halted => (
            "  HALTED - order flow blocked (adminHalt armed)  ".to_string(),
            Style::default()
                .fg(Color::White)
                .bg(Color::Red)
                .add_modifier(Modifier::BOLD),
        ),
        Some(_) => (
            "  LIVE - order flow enabled  ".to_string(),
            Style::default()
                .fg(Color::Black)
                .bg(Color::Green)
                .add_modifier(Modifier::BOLD),
        ),
    };
    let mut spans = vec![Span::styled(text, style)];
    if let Some(r) = hub
        && r.is_stale()
    {
        spans.push(Span::styled(
            format!("  STALE {} (last-known)", format_age(r.age)),
            Style::default()
                .fg(Color::Yellow)
                .add_modifier(Modifier::BOLD),
        ));
    }
    Paragraph::new(Line::from(spans))
        .block(Block::default().title("halt state").borders(Borders::ALL))
}

fn account_ratio(used: i64, cap: i64) -> f64 {
    if cap <= 0 {
        return 0.0;
    }
    (used as f64 / cap as f64).clamp(0.0, 1.0)
}

fn render_account(frame: &mut Frame, area: Rect, hub: &Option<HubReport>) {
    let block = Block::default()
        .title("account notional")
        .borders(Borders::ALL);
    let report = match hub {
        Some(r) => r,
        None => {
            frame.render_widget(
                Paragraph::new(Line::from(Span::styled(
                    "hub offline",
                    Style::default().fg(Color::DarkGray),
                )))
                .block(block),
                area,
            );
            return;
        }
    };
    let cap = report.status.max_account_notional_cents;
    let used = report.status.account_day_realized_cents + report.status.account_open_notional_cents;
    if cap <= 0 {
        frame.render_widget(
            Paragraph::new(Line::from(vec![
                Span::styled("used ", Style::default().fg(Color::DarkGray)),
                Span::raw(ntd(used)),
                Span::styled(
                    "   cap disabled (no account limit)",
                    Style::default().fg(Color::Yellow),
                ),
            ]))
            .block(block),
            area,
        );
        return;
    }
    let ratio = account_ratio(used, cap);
    let pct = used as f64 / cap as f64 * 100.0;
    let color = if pct >= 100.0 {
        Color::Red
    } else if pct >= 80.0 {
        Color::Yellow
    } else {
        Color::Green
    };
    let label = format!(
        "{} / {} ({:.1}%)  realized {}  open {}",
        ntd(used),
        ntd(cap),
        pct,
        ntd(report.status.account_day_realized_cents),
        ntd(report.status.account_open_notional_cents),
    );
    frame.render_widget(
        Gauge::default()
            .block(block)
            .gauge_style(Style::default().fg(color))
            .ratio(ratio)
            .label(label),
        area,
    );
}

fn open_order_lines(hub: &Option<HubReport>) -> Vec<Line<'static>> {
    let report = match hub {
        Some(r) => r,
        None => {
            return vec![Line::from(Span::styled(
                "-",
                Style::default().fg(Color::DarkGray),
            ))];
        }
    };
    let active: Vec<_> = report
        .status
        .clients
        .iter()
        .filter(|c| c.open > 0)
        .collect();
    if active.is_empty() {
        return vec![Line::from(Span::styled(
            "no open orders",
            Style::default().fg(Color::DarkGray),
        ))];
    }
    active
        .iter()
        .map(|c| {
            Line::from(format!(
                "{:<12} {:>4} open   {:>5} filled",
                c.prefix, c.open, c.filled
            ))
        })
        .collect()
}

fn blacklist_lines(state: &Fetch<BlacklistFreshness>) -> Vec<Line<'static>> {
    let red = Style::default().fg(Color::Red).add_modifier(Modifier::BOLD);
    let f = match state {
        Fetch::Loading => {
            return vec![Line::from(Span::styled(
                "loading...",
                Style::default().fg(Color::DarkGray),
            ))];
        }
        Fetch::Err(e) => return vec![Line::from(Span::styled(format!("ERROR: {e}"), red))],
        Fetch::Ok(f) => f,
    };
    if !f.present {
        return vec![Line::from(Span::styled(
            "missing (traders fail closed)",
            red,
        ))];
    }
    if f.malformed {
        return vec![Line::from(Span::styled(
            "malformed (traders fail closed)",
            red,
        ))];
    }
    if f.clock_anomaly {
        return vec![Line::from(Span::styled(
            "CLOCK ANOMALY: mtime in future (traders fail closed)",
            red,
        ))];
    }
    let age = f.age.map(format_age).unwrap_or_else(|| "?".to_string());
    let badge = if f.stale {
        Span::styled(format!("STALE {age} (traders fail closed)"), red)
    } else {
        Span::styled(format!("fresh {age}"), Style::default().fg(Color::Green))
    };
    let mut lines = vec![Line::from(vec![
        badge,
        Span::raw(format!("  {} restrictions", f.entries.len())),
    ])];
    for e in f.entries.iter().take(MAX_BLACKLIST_ROWS) {
        lines.push(Line::from(format!("  {:<10} {}", e.symbol, e.category)));
    }
    if f.entries.len() > MAX_BLACKLIST_ROWS {
        lines.push(Line::from(Span::styled(
            format!("  +{} more", f.entries.len() - MAX_BLACKLIST_ROWS),
            Style::default().fg(Color::DarkGray),
        )));
    }
    lines
}

pub fn render(
    frame: &mut Frame,
    area: Rect,
    hub: &Option<HubReport>,
    blacklist: &Fetch<BlacklistFreshness>,
) {
    let rows = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3),
            Constraint::Length(3),
            Constraint::Length(6),
            Constraint::Min(0),
        ])
        .split(area);

    frame.render_widget(halt_banner(hub), rows[0]);
    render_account(frame, rows[1], hub);
    frame.render_widget(
        Paragraph::new(open_order_lines(hub)).block(
            Block::default()
                .title("open orders (by scenario)")
                .borders(Borders::ALL),
        ),
        rows[2],
    );
    frame.render_widget(
        Paragraph::new(blacklist_lines(blacklist))
            .block(Block::default().title("blacklist").borders(Borders::ALL)),
        rows[3],
    );
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sources::blacklist::BlacklistEntry;
    use crate::sources::hub_status::HubStatus;
    use ratatui::Terminal;
    use ratatui::backend::TestBackend;
    use std::time::Duration;

    fn report(halted: bool, cap: i64, used_open: i64, realized: i64, age_s: u64) -> HubReport {
        HubReport {
            status: HubStatus {
                halted,
                max_account_notional_cents: cap,
                account_open_notional_cents: used_open,
                account_day_realized_cents: realized,
                ..Default::default()
            },
            age: Duration::from_secs(age_s),
        }
    }

    fn draw(hub: &Option<HubReport>, bl: &Fetch<BlacklistFreshness>) {
        let mut term = Terminal::new(TestBackend::new(100, 30)).unwrap();
        term.draw(|f| render(f, f.area(), hub, bl)).unwrap();
    }

    #[test]
    fn ratio_clamps_over_cap() {
        assert_eq!(account_ratio(200, 100), 1.0);
        assert_eq!(account_ratio(-50, 100), 0.0);
        assert_eq!(account_ratio(50, 0), 0.0);
    }

    #[test]
    fn renders_connected_with_cap() {
        let hub = Some(report(false, 50_000_000, 1_000_000, 500_000, 1));
        draw(&hub, &Fetch::Ok(BlacklistFreshness::default()));
    }

    #[test]
    fn renders_cap_disabled() {
        let hub = Some(report(false, 0, 1_000_000, 0, 1));
        draw(&hub, &Fetch::Ok(BlacklistFreshness::default()));
    }

    #[test]
    fn renders_halted_banner() {
        let hub = Some(report(true, 50_000_000, 0, 0, 1));
        draw(&hub, &Fetch::Loading);
    }

    #[test]
    fn renders_over_cap_without_panic() {
        let hub = Some(report(false, 100, 1_000, 0, 1));
        draw(&hub, &Fetch::Ok(BlacklistFreshness::default()));
    }

    #[test]
    fn renders_offline_and_stale() {
        draw(&None, &Fetch::Ok(BlacklistFreshness::default()));
        let hub = Some(report(true, 50_000_000, 0, 0, 30));
        draw(&hub, &Fetch::Err("boom".to_string()));
    }

    #[test]
    fn renders_populated_blacklist() {
        let bl = BlacklistFreshness {
            present: true,
            entries: (0..12)
                .map(|i| BlacklistEntry {
                    symbol: format!("{i:04}"),
                    category: "disposal".to_string(),
                })
                .collect(),
            entry_count: Some(12),
            ..Default::default()
        };
        draw(&Some(report(false, 1, 0, 0, 1)), &Fetch::Ok(bl));
    }
}
