use std::time::{Duration, SystemTime, UNIX_EPOCH};

use ratatui::Frame;
use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph};

use crate::sources::age::format_age;
use crate::sources::hub_status::HubReport;
use crate::sources::order_journal::{ScenarioJournal, ScenariosView};
use crate::sources::scenario_ctl::{
    Focus, RunningTrader, ScenarioPrompt, ScenarioToml, ScenarioUi,
};

const HUB_HEIGHT: u16 = 9;
const ACTIONS_HEIGHT: u16 = 5;
const RUNNING_HEIGHT: u16 = 8;
const TODAY_WIDTH: u16 = 50;

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

fn mode_span(live: bool) -> Span<'static> {
    if live {
        Span::styled(
            "LIVE",
            Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
        )
    } else {
        Span::styled("PAPER", Style::default().fg(Color::Green))
    }
}

fn select_line(cells: Vec<Span<'static>>, selected: bool) -> Line<'static> {
    let marker = if selected { "> " } else { "  " };
    let mut spans = vec![Span::raw(marker)];
    spans.extend(cells);
    let mut line = Line::from(spans);
    if selected {
        line = line.style(Style::default().add_modifier(Modifier::REVERSED));
    }
    line
}

fn available_lines(avail: &[ScenarioToml], skipped: usize, ui: &ScenarioUi) -> Vec<Line<'static>> {
    let mut lines = vec![Line::from(Span::styled(
        format!("  {:<22} {:<8} {:>6}", "toml", "symbol", "mode"),
        Style::default().fg(Color::DarkGray),
    ))];
    if avail.is_empty() {
        lines.push(dim("no scenario tomls in scenario dir"));
    }
    let focused = ui.focus == Focus::Available;
    let sel = ui.avail_sel.min(avail.len().saturating_sub(1));
    for (i, s) in avail.iter().enumerate() {
        let stem = s
            .path
            .file_stem()
            .map(|x| x.to_string_lossy().to_string())
            .unwrap_or_default();
        let cells = vec![
            Span::raw(format!("{stem:<22} ")),
            Span::raw(format!("{:<8} ", s.symbol)),
            mode_span(s.live),
        ];
        lines.push(select_line(cells, focused && i == sel));
    }
    if skipped > 0 {
        lines.push(dim(&format!("({skipped} unreadable toml skipped)")));
    }
    lines
}

fn running_lines(running: &[RunningTrader], ui: &ScenarioUi) -> Vec<Line<'static>> {
    let mut lines = vec![Line::from(Span::styled(
        format!("  {:>7} {:<26} {:>5}", "pid", "toml", "mode"),
        Style::default().fg(Color::DarkGray),
    ))];
    if running.is_empty() {
        lines.push(dim("no running scenario traders"));
    }
    let focused = ui.focus == Focus::Running;
    let sel = ui.run_sel.min(running.len().saturating_sub(1));
    for (i, t) in running.iter().enumerate() {
        let toml = t.toml.rsplit('/').next().unwrap_or(&t.toml).to_string();
        let cells = vec![
            Span::raw(format!("{:>7} ", t.pid)),
            Span::raw(format!("{toml:<26} ")),
            mode_span(t.live),
        ];
        lines.push(select_line(cells, focused && i == sel));
    }
    lines
}

fn legend_line() -> Line<'static> {
    Line::from(Span::styled(
        "[left/right] focus  [up/down] select  [s]tart  [x]stop   LIVE start needs typed confirm",
        Style::default().fg(Color::DarkGray),
    ))
}

fn actions_lines(ui: &ScenarioUi) -> Vec<Line<'static>> {
    let cyan = Style::default()
        .fg(Color::Cyan)
        .add_modifier(Modifier::BOLD);
    let red = Style::default().fg(Color::Red).add_modifier(Modifier::BOLD);
    let yellow = Style::default()
        .fg(Color::Yellow)
        .add_modifier(Modifier::BOLD);
    let mut lines = match &ui.confirm {
        ScenarioPrompt::Idle => vec![legend_line()],
        ScenarioPrompt::TypedStart {
            name,
            symbol,
            stem,
            buf,
            ..
        } => vec![
            Line::from(Span::styled(
                format!("START LIVE {name} ({symbol}) — REAL orders"),
                red,
            )),
            Line::from(Span::styled(
                format!("type '{stem}' and Enter to confirm   [Esc] cancel"),
                red,
            )),
            Line::from(vec![Span::raw("> "), Span::styled(buf.clone(), cyan)]),
        ],
        ScenarioPrompt::SimpleStart { name, symbol, .. } => vec![Line::from(Span::styled(
            format!("START PAPER {name} ({symbol})?  [y/N]"),
            yellow,
        ))],
        ScenarioPrompt::SimpleStop { pid, toml, live } => {
            let mode = if *live { "LIVE" } else { "PAPER" };
            let toml = toml.rsplit('/').next().unwrap_or(toml);
            vec![Line::from(Span::styled(
                format!("STOP pid {pid} {toml} ({mode})?  [y/N]"),
                yellow,
            ))]
        }
    };
    if let Some(msg) = &ui.last_result {
        lines.push(Line::from(Span::styled(
            msg.clone(),
            Style::default().fg(Color::DarkGray),
        )));
    }
    lines
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
            "{:<16} {:>6} {:>10} {:>6}",
            "journal", "fills", "shares", "last"
        ),
        Style::default().fg(Color::DarkGray),
    ))];
    if scenarios.is_empty() {
        lines.push(dim("no journal files for today"));
        return lines;
    }
    for s in scenarios {
        lines.push(Line::from(format!(
            "{:<16} {:>6} {:>10} {:>6}",
            s.name,
            s.fills,
            s.filled_shares,
            age_from_us(now, s.last_event_us),
        )));
    }
    lines
}

fn hub_lines(hub: &Option<HubReport>) -> Vec<Line<'static>> {
    let now_s = now_us() / 1_000_000;
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
        let idle = age_from_us(now_s * 1_000_000, c.last_activity_s * 1_000_000);
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

pub fn render(
    frame: &mut Frame,
    area: Rect,
    view: &ScenariosView,
    avail: &(Vec<ScenarioToml>, usize),
    running: &[RunningTrader],
    ui: &ScenarioUi,
) {
    let rows = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Min(0),
            Constraint::Length(RUNNING_HEIGHT),
            Constraint::Length(ACTIONS_HEIGHT),
            Constraint::Length(HUB_HEIGHT),
        ])
        .split(area);

    frame.render_widget(
        Paragraph::new(available_lines(&avail.0, avail.1, ui)).block(
            Block::default()
                .title("available scenarios")
                .borders(Borders::ALL),
        ),
        rows[0],
    );
    frame.render_widget(
        Paragraph::new(running_lines(running, ui)).block(
            Block::default()
                .title("running traders")
                .borders(Borders::ALL),
        ),
        rows[1],
    );
    frame.render_widget(
        Paragraph::new(actions_lines(ui))
            .block(Block::default().title("actions").borders(Borders::ALL)),
        rows[2],
    );

    let bottom = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Length(TODAY_WIDTH), Constraint::Min(0)])
        .split(rows[3]);
    frame.render_widget(
        Paragraph::new(scenario_lines(&view.scenarios, now_us())).block(
            Block::default()
                .title("scenarios (today)")
                .borders(Borders::ALL),
        ),
        bottom[0],
    );
    frame.render_widget(
        Paragraph::new(hub_lines(&view.hub))
            .block(Block::default().title("order hub").borders(Borders::ALL)),
        bottom[1],
    );
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sources::hub_status::{HubReport, HubStatus};
    use ratatui::Terminal;
    use ratatui::backend::TestBackend;
    use std::path::PathBuf;

    fn scen(stem: &str, symbol: &str, live: bool) -> ScenarioToml {
        ScenarioToml {
            path: PathBuf::from(format!("/e/{stem}.toml")),
            name: format!("{symbol}-plan"),
            symbol: symbol.to_string(),
            live,
        }
    }

    fn trader(pid: i32, live: bool) -> RunningTrader {
        RunningTrader {
            pid,
            toml: "/e/2330.toml".to_string(),
            live,
        }
    }

    fn draw(
        w: u16,
        h: u16,
        view: &ScenariosView,
        avail: &(Vec<ScenarioToml>, usize),
        running: &[RunningTrader],
        ui: &ScenarioUi,
    ) {
        let mut term = Terminal::new(TestBackend::new(w, h)).unwrap();
        term.draw(|f| render(f, f.area(), view, avail, running, ui))
            .unwrap();
    }

    #[test]
    fn renders_empty_without_panic() {
        let view = ScenariosView::default();
        draw(100, 30, &view, &(vec![], 0), &[], &ScenarioUi::default());
    }

    #[test]
    fn renders_many_with_skipped_and_no_hub() {
        let avail: Vec<_> = (0..40)
            .map(|i| scen(&format!("s{i}"), &format!("{i:04}"), i % 2 == 0))
            .collect();
        let running: Vec<_> = (0..6).map(|i| trader(1000 + i, i % 2 == 0)).collect();
        let ui = ScenarioUi {
            focus: Focus::Available,
            avail_sel: 39,
            run_sel: 5,
            ..Default::default()
        };
        draw(
            100,
            30,
            &ScenariosView::default(),
            &(avail, 3),
            &running,
            &ui,
        );
    }

    #[test]
    fn renders_confirm_overlays() {
        let ui_live = ScenarioUi {
            confirm: ScenarioPrompt::TypedStart {
                toml: PathBuf::from("/e/2330.toml"),
                name: "2330-plan".to_string(),
                symbol: "2330".to_string(),
                stem: "2330".to_string(),
                buf: "23".to_string(),
            },
            last_result: Some("launched pid 4242".to_string()),
            ..Default::default()
        };
        draw(
            80,
            20,
            &ScenariosView::default(),
            &(vec![], 0),
            &[],
            &ui_live,
        );

        let ui_stop = ScenarioUi {
            focus: Focus::Running,
            confirm: ScenarioPrompt::SimpleStop {
                pid: 4242,
                toml: "/e/2330.toml".to_string(),
                live: true,
            },
            ..Default::default()
        };
        draw(
            80,
            20,
            &ScenariosView::default(),
            &(vec![], 0),
            &[],
            &ui_stop,
        );
    }

    #[test]
    fn renders_with_hub_present() {
        let view = ScenariosView {
            scenarios: vec![],
            hub: Some(HubReport {
                status: HubStatus {
                    client_count: 1,
                    ..Default::default()
                },
                age: Duration::from_secs(1),
            }),
        };
        draw(100, 30, &view, &(vec![], 0), &[], &ScenarioUi::default());
    }
}
