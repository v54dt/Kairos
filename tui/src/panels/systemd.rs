use ratatui::Frame;
use ratatui::layout::Rect;
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

fn confirm_lines(ui: &ServiceUi) -> Vec<Line<'static>> {
    let cyan = Style::default()
        .fg(Color::Cyan)
        .add_modifier(Modifier::BOLD);
    let mut lines = match &ui.confirm {
        ConfirmPrompt::Idle => Vec::new(),
        ConfirmPrompt::TypedConfirm { verb, unit, buf } => {
            let stem = unit.rsplit_once('.').map(|(s, _)| s).unwrap_or(unit);
            vec![
                Line::from(Span::styled(
                    format!(
                        "type '{stem}' and Enter to {} {unit}   [Esc] cancel",
                        verb.as_str()
                    ),
                    Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
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
    let block = Block::default()
        .title("systemd (kairos-*)")
        .borders(Borders::ALL);
    let mut lines: Vec<Line> = match state {
        Fetch::Loading => vec![loading_line()],
        Fetch::Err(e) => vec![error_line(e)],
        Fetch::Ok(units) if units.is_empty() => {
            vec![Line::from(Span::styled(
                "no kairos-* units",
                Style::default().fg(Color::DarkGray),
            ))]
        }
        Fetch::Ok(units) => {
            let sel = ui.selected.min(units.len().saturating_sub(1));
            units
                .iter()
                .enumerate()
                .map(|(i, u)| unit_line(u, i == sel))
                .collect()
        }
    };
    lines.push(Line::from(""));
    lines.push(legend_line());
    lines.extend(confirm_lines(ui));
    frame.render_widget(Paragraph::new(lines).block(block), area);
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
}
