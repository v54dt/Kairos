use std::time::{Duration, SystemTime, UNIX_EPOCH};

use ratatui::Frame;
use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph};

use crate::sources::age::format_age;
use crate::sources::hub_status::HubReport;
use crate::sources::order_journal::{ScenarioJournal, ScenariosView};
use crate::sources::scenario_ctl::{ScenarioPrompt, ScenarioUi};
use crate::sources::supervisor::{SupervisorRow, SupervisorState};

const HUB_HEIGHT: u16 = 9;
const ACTIONS_HEIGHT: u16 = 5;
const TODAY_WIDTH: u16 = 50;
const NAME_WIDTH: usize = 18;
const STATE_WIDTH: usize = 14;

/// Vertical scroll so the 0-indexed selected data row stays visible inside a
/// `view_h`-tall list area (header rendered separately, so no border/header
/// adjustment here).
fn scroll_offset(view_h: usize, sel: usize) -> u16 {
    if view_h > 0 && sel >= view_h {
        (sel - view_h + 1) as u16
    } else {
        0
    }
}

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

fn orders_span(live: bool) -> Span<'static> {
    if live {
        Span::styled(
            format!("{:<5}", "LIVE"),
            Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
        )
    } else {
        Span::styled(format!("{:<5}", "PAPER"), Style::default().fg(Color::Green))
    }
}

fn dot_span(running: bool) -> Span<'static> {
    let color = if running { Color::Green } else { Color::Red };
    Span::styled("\u{25cf}", Style::default().fg(color))
}

fn trunc(s: &str, n: usize) -> String {
    if s.chars().count() <= n {
        return s.to_string();
    }
    let mut out: String = s.chars().take(n.saturating_sub(1)).collect();
    out.push('\u{2026}');
    out
}

fn header_line() -> Line<'static> {
    Line::from(Span::styled(
        format!(
            "    {:<n$} {:<st$} {:<5} {:>7} {:>6}  {}",
            "scenario",
            "state",
            "order",
            "pid",
            "fills",
            "reason",
            n = NAME_WIDTH,
            st = STATE_WIDTH,
        ),
        Style::default().fg(Color::DarkGray),
    ))
}

fn row_line(row: &SupervisorRow, selected: bool) -> Line<'static> {
    let marker = if selected { "> " } else { "  " };
    let pid = if row.state.is_running() && row.pid > 0 {
        format!("pid {}", row.pid)
    } else {
        String::new()
    };
    let mut line = Line::from(vec![
        Span::raw(marker),
        dot_span(row.state.is_running()),
        Span::raw(format!(
            " {:<n$} ",
            trunc(&row.name, NAME_WIDTH),
            n = NAME_WIDTH
        )),
        Span::raw(format!(
            "{:<st$} ",
            trunc(&row.state.name(), STATE_WIDTH),
            st = STATE_WIDTH
        )),
        orders_span(row.live),
        Span::raw(format!(" {pid:>7} {:>6}  ", row.cum_fills)),
        Span::styled(
            trunc(&row.last_exit_reason, 40),
            Style::default().fg(Color::DarkGray),
        ),
    ]);
    if selected {
        line = line.style(Style::default().add_modifier(Modifier::REVERSED));
    }
    line
}

fn data_lines(sup: &SupervisorState, sel: usize) -> Vec<Line<'static>> {
    if !sup.connected {
        return vec![
            Line::from(Span::styled(
                "supervisor not connected — is kairos_scenario_supervisord running?",
                Style::default()
                    .fg(Color::Yellow)
                    .add_modifier(Modifier::BOLD),
            )),
            dim(sup.last_error.as_deref().unwrap_or("no reply from daemon")),
        ];
    }
    if sup.rows.is_empty() {
        return vec![dim("no scenarios reported by the supervisor")];
    }
    sup.rows
        .iter()
        .enumerate()
        .map(|(i, r)| row_line(r, i == sel))
        .collect()
}

fn footer_line(total: usize, offset: usize, view_h: usize) -> Line<'static> {
    if total == 0 {
        return dim("showing 0\u{2013}0 of 0");
    }
    let first = offset + 1;
    let last = (offset + view_h.max(1)).min(total);
    dim(&format!("showing {first}\u{2013}{last} of {total}"))
}

fn legend_line() -> Line<'static> {
    Line::from(Span::styled(
        "[up/down] select  [PgUp/PgDn] page  [s]tart paper  [l]ive  [t]est  [x]stop   LIVE needs typed confirm",
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
        ScenarioPrompt::TypedStart { name, buf } => vec![
            Line::from(Span::styled(
                format!("START LIVE {name} — REAL orders"),
                red,
            )),
            Line::from(Span::styled(
                format!("type '{name}' and Enter to confirm   [Esc] cancel"),
                red,
            )),
            Line::from(vec![Span::raw("> "), Span::styled(buf.clone(), cyan)]),
        ],
        ScenarioPrompt::SimpleStart { name, mode } => {
            let label = if *mode == crate::sources::supervisor::Mode::Test {
                "TEST"
            } else {
                "PAPER"
            };
            vec![Line::from(Span::styled(
                format!("START {label} {name}?  [y/N]"),
                yellow,
            ))]
        }
        ScenarioPrompt::SimpleStop { name, live } => {
            let mode = if *live { "LIVE" } else { "PAPER" };
            vec![Line::from(Span::styled(
                format!("STOP {name} ({mode})?  [y/N]"),
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
    sup: &SupervisorState,
    ui: &ScenarioUi,
) {
    let rows = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Min(0),
            Constraint::Length(ACTIONS_HEIGHT),
            Constraint::Length(HUB_HEIGHT),
        ])
        .split(area);

    let total = sup.rows.len();
    let sel = ui.sel.min(total.saturating_sub(1));

    let list_block = Block::default().title("scenarios").borders(Borders::ALL);
    let inner = list_block.inner(rows[0]);
    frame.render_widget(list_block, rows[0]);
    let parts = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(1),
            Constraint::Min(0),
            Constraint::Length(1),
        ])
        .split(inner);
    frame.render_widget(Paragraph::new(header_line()), parts[0]);
    let view_h = parts[1].height as usize;
    let offset = scroll_offset(view_h, sel);
    frame.render_widget(
        Paragraph::new(data_lines(sup, sel)).scroll((offset, 0)),
        parts[1],
    );
    frame.render_widget(
        Paragraph::new(footer_line(total, offset as usize, view_h)),
        parts[2],
    );

    frame.render_widget(
        Paragraph::new(actions_lines(ui))
            .block(Block::default().title("actions").borders(Borders::ALL)),
        rows[1],
    );

    let bottom = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Length(TODAY_WIDTH), Constraint::Min(0)])
        .split(rows[2]);
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
    use crate::sources::supervisor::ScenarioState;
    use ratatui::Terminal;
    use ratatui::backend::TestBackend;

    fn row(
        name: &str,
        state: ScenarioState,
        pid: i64,
        fills: i64,
        reason: &str,
        live: bool,
    ) -> SupervisorRow {
        SupervisorRow {
            name: name.to_string(),
            state,
            pid,
            cum_fills: fills,
            cum_shares: fills * 1000,
            last_fill_ts: 0,
            last_exit_reason: reason.to_string(),
            live,
        }
    }

    fn connected(rows: Vec<SupervisorRow>) -> SupervisorState {
        SupervisorState {
            connected: true,
            last_error: None,
            rows,
        }
    }

    fn draw(w: u16, h: u16, view: &ScenariosView, sup: &SupervisorState, ui: &ScenarioUi) {
        let mut term = Terminal::new(TestBackend::new(w, h)).unwrap();
        term.draw(|f| render(f, f.area(), view, sup, ui)).unwrap();
    }

    fn buffer_text(
        w: u16,
        h: u16,
        view: &ScenariosView,
        sup: &SupervisorState,
        ui: &ScenarioUi,
    ) -> String {
        let mut term = Terminal::new(TestBackend::new(w, h)).unwrap();
        term.draw(|f| render(f, f.area(), view, sup, ui)).unwrap();
        let buf = term.backend().buffer().clone();
        let area = buf.area;
        let mut s = String::new();
        for y in 0..area.height {
            for x in 0..area.width {
                s.push_str(buf[(x, y)].symbol());
            }
            s.push('\n');
        }
        s
    }

    #[test]
    fn renders_empty_without_panic() {
        draw(
            100,
            30,
            &ScenariosView::default(),
            &connected(vec![]),
            &ScenarioUi::default(),
        );
    }

    #[test]
    fn not_connected_shows_banner() {
        let sup = SupervisorState {
            connected: false,
            last_error: Some("connection refused".to_string()),
            ..Default::default()
        };
        let text = buffer_text(
            100,
            30,
            &ScenariosView::default(),
            &sup,
            &ScenarioUi::default(),
        );
        assert!(
            text.contains("supervisor not connected"),
            "not-connected banner missing:\n{text}"
        );
    }

    #[test]
    fn running_row_shows_green_dot_pid_state_and_live() {
        let sup = connected(vec![row(
            "2330",
            ScenarioState::InWindow,
            4242,
            3,
            "",
            true,
        )]);
        let text = buffer_text(
            120,
            30,
            &ScenariosView::default(),
            &sup,
            &ScenarioUi::default(),
        );
        assert!(
            text.contains('\u{25cf}'),
            "row must show a status dot:\n{text}"
        );
        assert!(
            text.contains("pid 4242"),
            "running row must show its pid:\n{text}"
        );
        assert!(
            text.contains("in-window"),
            "row must show the state name:\n{text}"
        );
        assert!(text.contains("LIVE"), "orders column shows LIVE:\n{text}");
    }

    #[test]
    fn closed_exited_row_shows_reason_and_no_pid() {
        let sup = connected(vec![row(
            "2454",
            ScenarioState::ClosedExited,
            0,
            5,
            "window closed / run complete",
            false,
        )]);
        let text = buffer_text(
            120,
            30,
            &ScenariosView::default(),
            &sup,
            &ScenarioUi::default(),
        );
        assert!(text.contains("closed-exited"), "state name shown:\n{text}");
        assert!(
            text.contains("window closed"),
            "self-exit reason must be visible:\n{text}"
        );
        assert_eq!(
            text.matches("pid").count(),
            1,
            "only the header 'pid' appears; a stopped row shows no pid value:\n{text}"
        );
        assert!(text.contains("PAPER"), "orders column shows PAPER:\n{text}");
    }

    #[test]
    fn footer_shows_visible_range_of_total() {
        let rows: Vec<_> = (0..40)
            .map(|i| row(&format!("s{i}"), ScenarioState::Stopped, 0, 0, "", false))
            .collect();
        let text = buffer_text(
            100,
            30,
            &ScenariosView::default(),
            &connected(rows),
            &ScenarioUi::default(),
        );
        assert!(text.contains("showing 1"), "footer start missing:\n{text}");
        assert!(text.contains("of 40"), "footer total missing:\n{text}");
    }

    #[test]
    fn selection_scrolls_into_view_on_small_terminal() {
        let rows: Vec<_> = (0..40)
            .map(|i| row(&format!("s{i}"), ScenarioState::Stopped, 0, 0, "", false))
            .collect();
        let ui = ScenarioUi {
            sel: 39,
            ..Default::default()
        };
        let text = buffer_text(80, 24, &ScenariosView::default(), &connected(rows), &ui);
        assert!(
            text.contains("s39 "),
            "selected row scrolled off-screen:\n{text}"
        );
        assert!(
            text.contains("[up/down] select"),
            "action/confirm banner squeezed out:\n{text}"
        );
    }

    #[test]
    fn renders_confirm_overlays() {
        let ui_live = ScenarioUi {
            confirm: ScenarioPrompt::TypedStart {
                name: "2330".to_string(),
                buf: "23".to_string(),
            },
            last_result: Some("start 2330 (live): ok".to_string()),
            ..Default::default()
        };
        draw(
            80,
            20,
            &ScenariosView::default(),
            &connected(vec![]),
            &ui_live,
        );

        let ui_stop = ScenarioUi {
            confirm: ScenarioPrompt::SimpleStop {
                name: "2330".to_string(),
                live: true,
            },
            ..Default::default()
        };
        draw(
            80,
            20,
            &ScenariosView::default(),
            &connected(vec![]),
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
        draw(100, 30, &view, &connected(vec![]), &ScenarioUi::default());
    }
}
