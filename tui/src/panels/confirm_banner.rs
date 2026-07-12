//! The byte-identical parts of the three confirm banners (risk kill switch,
//! systemd actions, scenarios actions): the typed-confirm block and the dim
//! last-result tail. Panel-specific legends and y/N one-liners stay in the panels.

use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};

/// The shared typed-confirm block: an optional action headline, the instruction
/// line, then the `> buf` echo (bold cyan). The risk kill switch passes
/// `action = None` (two lines); systemd/scenarios pass `Some` (three lines).
pub(crate) fn typed_confirm_lines(
    action: Option<Line<'static>>,
    instruction: Line<'static>,
    buf: &str,
) -> Vec<Line<'static>> {
    let cyan = Style::default()
        .fg(Color::Cyan)
        .add_modifier(Modifier::BOLD);
    let mut lines = Vec::new();
    if let Some(a) = action {
        lines.push(a);
    }
    lines.push(instruction);
    lines.push(Line::from(vec![
        Span::raw("> "),
        Span::styled(buf.to_string(), cyan),
    ]));
    lines
}

/// Append the dim last-result tail shared by the three confirm panels.
pub(crate) fn with_result(
    mut lines: Vec<Line<'static>>,
    result: &Option<String>,
) -> Vec<Line<'static>> {
    if let Some(msg) = result {
        lines.push(Line::from(Span::styled(
            msg.clone(),
            Style::default().fg(Color::DarkGray),
        )));
    }
    lines
}
