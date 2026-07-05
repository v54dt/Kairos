use ratatui::Frame;
use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph};

use crate::app::Fetch;
use crate::panels::{error_line, loading_line};
use crate::sources::service::{ConfirmPrompt, ServiceUi};
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

fn unit_line(u: &UnitStatus, selected: bool) -> Line<'static> {
    let color = active_color(&u.active);
    let marker = if selected { "> " } else { "  " };
    let mut line = Line::from(vec![
        Span::raw(format!("{marker}{:<26}", u.unit)),
        Span::styled(format!("{:<11}", u.active), Style::default().fg(color)),
        Span::styled(
            u.sub.clone(),
            Style::default().fg(color).add_modifier(Modifier::DIM),
        ),
    ]);
    if selected {
        line = line.style(Style::default().add_modifier(Modifier::REVERSED));
    }
    line
}

fn legend_line() -> Line<'static> {
    Line::from(Span::styled(
        "[up/down] select  [r]estart [s]tart [x]stop [f] reset-failed  [S]/[X] target up/down",
        Style::default().fg(Color::DarkGray),
    ))
}

fn footer_lines(ui: &ServiceUi) -> Vec<Line<'static>> {
    let cyan = Style::default()
        .fg(Color::Cyan)
        .add_modifier(Modifier::BOLD);
    let mut lines = match &ui.confirm {
        ConfirmPrompt::Idle => vec![legend_line()],
        ConfirmPrompt::TypedConfirm { verb, unit, buf } => {
            let stem = unit.rsplit_once('.').map(|(s, _)| s).unwrap_or(unit);
            let red = Style::default().fg(Color::Red).add_modifier(Modifier::BOLD);
            vec![
                Line::from(Span::styled(format!("{} {unit}", verb.as_str()), red)),
                Line::from(Span::styled(
                    format!("type '{stem}' and Enter to confirm   [Esc] cancel"),
                    red,
                )),
                Line::from(vec![Span::raw("> "), Span::styled(buf.clone(), cyan)]),
            ]
        }
        ConfirmPrompt::SimpleConfirm { verb, unit } => vec![Line::from(Span::styled(
            format!("{} {unit}?  [y/N]", verb.as_str()),
            Style::default()
                .fg(Color::Yellow)
                .add_modifier(Modifier::BOLD),
        ))],
    };
    if let Some(msg) = &ui.last_result {
        lines.push(Line::from(Span::styled(
            msg.clone(),
            Style::default().fg(Color::DarkGray),
        )));
    }
    lines
}

pub fn render(frame: &mut Frame, area: Rect, state: &Fetch<Vec<UnitStatus>>, ui: &ServiceUi) {
    let rows = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Min(0), Constraint::Length(6)])
        .split(area);

    let block = Block::default()
        .title("systemd (kairos-*)")
        .borders(Borders::ALL);
    let (lines, sel): (Vec<Line>, usize) = match state {
        Fetch::Loading => (vec![loading_line()], 0),
        Fetch::Err(e) => (vec![error_line(e)], 0),
        Fetch::Ok(units) if units.is_empty() => (
            vec![Line::from(Span::styled(
                "no kairos-* units",
                Style::default().fg(Color::DarkGray),
            ))],
            0,
        ),
        Fetch::Ok(units) => {
            let sel = ui.selected.min(units.len().saturating_sub(1));
            let lines = units
                .iter()
                .enumerate()
                .map(|(i, u)| unit_line(u, i == sel))
                .collect();
            (lines, sel)
        }
    };

    let inner_h = rows[0].height.saturating_sub(2) as usize;
    let offset = if inner_h > 0 && sel >= inner_h {
        (sel - inner_h + 1) as u16
    } else {
        0
    };
    frame.render_widget(
        Paragraph::new(lines).block(block).scroll((offset, 0)),
        rows[0],
    );
    frame.render_widget(
        Paragraph::new(footer_lines(ui))
            .block(Block::default().title("actions").borders(Borders::ALL)),
        rows[1],
    );
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sources::service::Verb;
    use ratatui::Terminal;
    use ratatui::backend::TestBackend;

    fn unit(name: &str, active: &str) -> UnitStatus {
        UnitStatus {
            unit: name.to_string(),
            load: "loaded".to_string(),
            active: active.to_string(),
            sub: "running".to_string(),
            description: "desc".to_string(),
        }
    }

    fn draw(state: &Fetch<Vec<UnitStatus>>, ui: &ServiceUi) {
        let mut term = Terminal::new(TestBackend::new(100, 30)).unwrap();
        term.draw(|f| render(f, f.area(), state, ui)).unwrap();
    }

    fn buffer_text(w: u16, h: u16, state: &Fetch<Vec<UnitStatus>>, ui: &ServiceUi) -> String {
        let mut term = Terminal::new(TestBackend::new(w, h)).unwrap();
        term.draw(|f| render(f, f.area(), state, ui)).unwrap();
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
    fn renders_all_states_without_panic() {
        let ui = ServiceUi::default();
        draw(&Fetch::Loading, &ui);
        draw(&Fetch::Err("boom".to_string()), &ui);
        draw(&Fetch::Ok(vec![]), &ui);
    }

    #[test]
    fn renders_many_units_with_selection() {
        let units: Vec<_> = (0..8)
            .map(|i| unit(&format!("kairos-lab-{i}.service"), "active"))
            .collect();
        let ui = ServiceUi {
            selected: 3,
            ..Default::default()
        };
        draw(&Fetch::Ok(units), &ui);
    }

    #[test]
    fn selection_past_end_is_clamped() {
        let units = vec![unit("kairos-core.service", "failed")];
        let ui = ServiceUi {
            selected: 99,
            ..Default::default()
        };
        draw(&Fetch::Ok(units), &ui);
    }

    #[test]
    fn renders_confirm_overlays() {
        let units = vec![unit("kairos-driver.service", "active")];
        let ui = ServiceUi {
            selected: 0,
            confirm: ConfirmPrompt::TypedConfirm {
                verb: Verb::Restart,
                unit: "kairos-driver.service".to_string(),
                buf: "kairos-dri".to_string(),
            },
            last_result: Some("restart kairos-driver.service ok".to_string()),
        };
        draw(&Fetch::Ok(units.clone()), &ui);
        let ui = ServiceUi {
            selected: 0,
            confirm: ConfirmPrompt::SimpleConfirm {
                verb: Verb::Start,
                unit: "kairos-bsr.service".to_string(),
            },
            last_result: None,
        };
        draw(&Fetch::Ok(units), &ui);
    }

    #[test]
    fn confirm_and_result_visible_with_many_units_on_small_terminal() {
        let units: Vec<_> = (0..17)
            .map(|i| unit(&format!("kairos-unit-{i}.service"), "active"))
            .collect();
        let ui = ServiceUi {
            selected: 1,
            confirm: ConfirmPrompt::TypedConfirm {
                verb: Verb::Stop,
                unit: "kairos-core.service".to_string(),
                buf: "kairos-co".to_string(),
            },
            last_result: Some("stop kairos-core.service failed: boom".to_string()),
        };
        let text = buffer_text(40, 9, &Fetch::Ok(units), &ui);
        assert!(
            text.contains("stop kairos-core.service"),
            "verb+unit clipped:\n{text}"
        );
        assert!(
            text.contains("type 'kairos-core'"),
            "confirm instruction clipped:\n{text}"
        );
        assert!(
            text.contains("> kairos-co"),
            "typed-buffer echo clipped:\n{text}"
        );
        assert!(
            text.contains("stop kairos-core.service failed"),
            "action result clipped:\n{text}"
        );
    }

    #[test]
    fn selection_scrolls_into_view_with_many_units() {
        let units: Vec<_> = (0..17)
            .map(|i| unit(&format!("kairos-unit-{i}.service"), "active"))
            .collect();
        let ui = ServiceUi {
            selected: 16,
            ..Default::default()
        };
        let text = buffer_text(40, 9, &Fetch::Ok(units), &ui);
        assert!(
            text.contains("kairos-unit-16"),
            "selected unit scrolled off-screen:\n{text}"
        );
    }
}
