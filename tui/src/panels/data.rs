use std::time::{Duration, SystemTime};

use ratatui::Frame;
use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph};

use crate::app::{Fetch, Snapshot};
use crate::panels::{error_line, human_bytes, loading_line};
use crate::sources::age::format_age;
use crate::sources::archive::{ArchiveScan, ShipVerify};
use crate::sources::blacklist::BlacklistFreshness;
use crate::sources::journald::{LogLine, Severity, classify_severity};
use crate::sources::timers::TimerEntry;

const GROWING_WINDOW: Duration = Duration::from_secs(60);

fn dim(text: &str) -> Line<'static> {
    Line::from(Span::styled(
        text.to_string(),
        Style::default().fg(Color::DarkGray),
    ))
}

fn short_unit(unit: &str) -> &str {
    unit.strip_suffix(".service")
        .or_else(|| unit.strip_suffix(".timer"))
        .unwrap_or(unit)
}

fn timer_lines(state: &Fetch<Vec<TimerEntry>>) -> Vec<Line<'static>> {
    let entries = match state {
        Fetch::Loading => return vec![loading_line()],
        Fetch::Err(e) => return vec![error_line(e)],
        Fetch::Ok(v) if v.is_empty() => return vec![dim("no kairos-* timers")],
        Fetch::Ok(v) => v,
    };
    let mut lines = vec![Line::from(Span::styled(
        format!(
            "{:<26} {:<14} {:<10} {}",
            "timer", "next", "left", "last-run"
        ),
        Style::default().fg(Color::DarkGray),
    ))];
    for e in entries {
        let last = match &e.result {
            None => Span::styled("n/a", Style::default().fg(Color::DarkGray)),
            Some(r) if r.exit_ts.is_empty() => {
                Span::styled("never", Style::default().fg(Color::DarkGray))
            }
            Some(r) => {
                let color = if r.exec_main_status == 0 {
                    Color::Green
                } else {
                    Color::Red
                };
                Span::styled(
                    format!("exit={} {}", r.exec_main_status, r.exit_ts),
                    Style::default().fg(color),
                )
            }
        };
        lines.push(Line::from(vec![
            Span::raw(format!(
                "{:<26} {:<14} {:<10} ",
                short_unit(&e.row.unit),
                e.row.next,
                e.row.left,
            )),
            last,
        ]));
    }
    lines
}

fn blacklist_lines(state: &Fetch<BlacklistFreshness>) -> Vec<Line<'static>> {
    let f = match state {
        Fetch::Loading => return vec![loading_line()],
        Fetch::Err(e) => return vec![error_line(e)],
        Fetch::Ok(f) => f,
    };
    let red = Style::default().fg(Color::Red).add_modifier(Modifier::BOLD);
    if !f.present {
        return vec![Line::from(vec![
            Span::styled("missing (traders fail closed)", red),
            Span::styled(
                format!("  {}", f.path),
                Style::default().fg(Color::DarkGray),
            ),
        ])];
    }
    if f.malformed {
        return vec![Line::from(vec![
            Span::styled("malformed (traders fail closed)", red),
            Span::styled(
                format!("  {}", f.path),
                Style::default().fg(Color::DarkGray),
            ),
        ])];
    }
    let age = f.age.map(format_age).unwrap_or_else(|| "?".to_string());
    let badge = if f.stale {
        Span::styled(format!("STALE {age} (traders fail closed)"), red)
    } else {
        Span::styled(format!("fresh {age}"), Style::default().fg(Color::Green))
    };
    let entries = f
        .entry_count
        .map(|c| format!("{c} entries"))
        .unwrap_or_else(|| "? entries".to_string());
    vec![
        Line::from(vec![badge, Span::raw(format!("  {entries}"))]),
        Line::from(Span::styled(
            f.path.clone(),
            Style::default().fg(Color::DarkGray),
        )),
    ]
}

fn archive_lines(
    archive: &Fetch<ArchiveScan>,
    ship: &Fetch<Option<ShipVerify>>,
    now: SystemTime,
) -> Vec<Line<'static>> {
    let mut lines = Vec::new();
    match archive {
        Fetch::Loading => lines.push(loading_line()),
        Fetch::Err(e) => lines.push(error_line(e)),
        Fetch::Ok(scan) => {
            if scan.today_files.is_empty() {
                lines.push(dim("today: no .kqr files"));
            } else {
                for kf in &scan.today_files {
                    let growing = now
                        .duration_since(kf.mtime)
                        .map(|d| d < GROWING_WINDOW)
                        .unwrap_or(false);
                    let (tag, style) = if growing {
                        ("growing", Style::default().fg(Color::Green))
                    } else {
                        ("idle", Style::default().fg(Color::DarkGray))
                    };
                    lines.push(Line::from(vec![
                        Span::raw(format!("{:<24} {:>10}  ", kf.name, human_bytes(kf.size))),
                        Span::styled(tag, style),
                    ]));
                }
            }
            let yday = if scan.yesterday_compressed {
                Span::styled(
                    "yesterday: .kqr.zst present",
                    Style::default().fg(Color::Green),
                )
            } else if scan.yesterday_uncompressed {
                Span::styled(
                    "yesterday: uncompressed .kqr (not shipped)",
                    Style::default().fg(Color::Yellow),
                )
            } else {
                Span::styled("yesterday: none", Style::default().fg(Color::DarkGray))
            };
            lines.push(Line::from(yday));
        }
    }
    match ship {
        Fetch::Loading => lines.push(loading_line()),
        Fetch::Err(e) => lines.push(error_line(e)),
        Fetch::Ok(None) => lines.push(dim("ship/verify: no record")),
        Fetch::Ok(Some(sv)) => {
            let (tag, style) = if sv.ok {
                ("OK", Style::default().fg(Color::Green))
            } else {
                (
                    "FAILED",
                    Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
                )
            };
            lines.push(Line::from(vec![
                Span::raw("ship/verify: "),
                Span::styled(tag, style),
                Span::styled(
                    format!("  {} {}", sv.ts, sv.detail),
                    Style::default().fg(Color::DarkGray),
                ),
            ]));
        }
    }
    lines
}

fn event_line(l: &LogLine) -> Line<'static> {
    let msg_style = match classify_severity(&l.message) {
        Severity::Error => Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
        Severity::Warn => Style::default().fg(Color::Yellow),
        Severity::Info => Style::default(),
    };
    Line::from(vec![
        Span::styled(format!("{} ", l.ts), Style::default().fg(Color::DarkGray)),
        Span::styled(
            format!("{:<18} ", short_unit(&l.unit)),
            Style::default().fg(Color::Cyan),
        ),
        Span::styled(l.message.clone(), msg_style),
    ])
}

fn event_lines(state: &Fetch<Vec<LogLine>>) -> Vec<Line<'static>> {
    match state {
        Fetch::Loading => vec![loading_line()],
        Fetch::Err(e) => vec![error_line(e)],
        Fetch::Ok(logs) if logs.is_empty() => vec![dim("no events")],
        Fetch::Ok(logs) => logs.iter().map(event_line).collect(),
    }
}

pub fn render(frame: &mut Frame, area: Rect, snap: &Snapshot) {
    let rows = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(8),
            Constraint::Length(4),
            Constraint::Length(7),
            Constraint::Min(0),
        ])
        .split(area);

    frame.render_widget(
        Paragraph::new(timer_lines(&snap.timers)).block(
            Block::default()
                .title("jobs & timers")
                .borders(Borders::ALL),
        ),
        rows[0],
    );
    frame.render_widget(
        Paragraph::new(blacklist_lines(&snap.blacklist)).block(
            Block::default()
                .title("blacklist freshness")
                .borders(Borders::ALL),
        ),
        rows[1],
    );
    frame.render_widget(
        Paragraph::new(archive_lines(
            &snap.archive,
            &snap.ship_verify,
            SystemTime::now(),
        ))
        .block(
            Block::default()
                .title("recorder archive")
                .borders(Borders::ALL),
        ),
        rows[2],
    );
    frame.render_widget(
        Paragraph::new(event_lines(&snap.events))
            .block(Block::default().title("events").borders(Borders::ALL)),
        rows[3],
    );
}
