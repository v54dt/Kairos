use ratatui::Frame;
use ratatui::layout::Rect;
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph};

use crate::app::Fetch;
use crate::panels::{error_line, loading_line};
use crate::sources::journald::{LogLine, Severity, classify_severity};

fn short_unit(unit: &str) -> &str {
    unit.strip_suffix(".service").unwrap_or(unit)
}

fn log_line(l: &LogLine) -> Line<'static> {
    Line::from(vec![
        Span::styled(format!("{} ", l.ts), Style::default().fg(Color::DarkGray)),
        Span::styled(
            format!("{:<18} ", short_unit(&l.unit)),
            Style::default().fg(Color::Cyan),
        ),
        Span::raw(l.message.clone()),
    ])
}

pub fn render(frame: &mut Frame, area: Rect, state: &Fetch<Vec<LogLine>>) {
    let block = Block::default().title("events").borders(Borders::ALL);
    let lines: Vec<Line> = match state {
        Fetch::Loading => vec![loading_line()],
        Fetch::Err(e) => vec![error_line(e)],
        Fetch::Ok(logs) if logs.is_empty() => vec![Line::from(Span::styled(
            "no events",
            Style::default().fg(Color::DarkGray),
        ))],
        Fetch::Ok(logs) => logs.iter().map(log_line).collect(),
    };
    frame.render_widget(Paragraph::new(lines).block(block), area);
}

/// A read-only snapshot of the journal drill-down: the target unit, the last
/// fetch result, and the scroll position expressed as lines-from-bottom.
pub struct DrillView {
    pub unit: String,
    pub state: Fetch<Vec<LogLine>>,
    pub rev: usize,
}

fn drill_line(l: &LogLine) -> Line<'static> {
    let style = match classify_severity(&l.message) {
        Severity::Error => Style::default().fg(Color::Red),
        Severity::Warn => Style::default().fg(Color::Yellow),
        Severity::Info => Style::default(),
    };
    Line::from(vec![
        Span::styled(format!("{} ", l.ts), Style::default().fg(Color::DarkGray)),
        Span::styled(l.message.clone(), style),
    ])
}

/// Largest meaningful `rev` (lines-from-bottom) for the given content: scrolling
/// past this only pins the view to the oldest line, so it is the clamp bound.
fn max_rev(total: usize, viewport: usize) -> usize {
    if viewport == 0 || total <= viewport {
        0
    } else {
        total - viewport
    }
}

/// Top scroll offset for a `rev` lines-from-bottom position: `rev == 0` pins the
/// newest line to the bottom; larger `rev` walks toward the top. All-saturating
/// so it never scrolls past either end.
fn scroll_top(total: usize, viewport: usize, rev: usize) -> u16 {
    let max_top = max_rev(total, viewport);
    (max_top - rev.min(max_top)).min(u16::MAX as usize) as u16
}

/// Renders the drill-down and returns the scroll position clamped to the
/// content actually rendered, so the caller can write it back and never
/// accumulate phantom over-scroll past the oldest line.
pub fn render_drilldown(frame: &mut Frame, area: Rect, view: &DrillView) -> usize {
    let mut title = vec![Span::styled(
        format!("journal: {}", view.unit),
        Style::default()
            .fg(Color::Cyan)
            .add_modifier(Modifier::BOLD),
    )];
    if matches!(view.state, Fetch::Err(_)) {
        title.push(Span::raw(" "));
        title.push(Span::styled(
            "[stale]",
            Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
        ));
    }
    let block = Block::default()
        .title(Line::from(title))
        .borders(Borders::ALL);
    let lines: Vec<Line> = match &view.state {
        Fetch::Loading => vec![loading_line()],
        Fetch::Err(e) => vec![error_line(e)],
        Fetch::Ok(logs) if logs.is_empty() => vec![Line::from(Span::styled(
            format!("no journal entries for {}", view.unit),
            Style::default().fg(Color::DarkGray),
        ))],
        Fetch::Ok(logs) => logs.iter().map(drill_line).collect(),
    };
    let viewport = area.height.saturating_sub(2) as usize;
    let rev = view.rev.min(max_rev(lines.len(), viewport));
    let top = scroll_top(lines.len(), viewport, rev);
    frame.render_widget(Paragraph::new(lines).block(block).scroll((top, 0)), area);
    rev
}

#[cfg(test)]
mod tests {
    use super::*;
    use ratatui::Terminal;
    use ratatui::backend::TestBackend;

    fn log(ts: &str, message: &str) -> LogLine {
        LogLine {
            ts: ts.to_string(),
            unit: "kairos-orderhub.service".to_string(),
            message: message.to_string(),
        }
    }

    fn buffer_text(w: u16, h: u16, view: &DrillView) -> String {
        let mut term = Terminal::new(TestBackend::new(w, h)).unwrap();
        term.draw(|f| {
            render_drilldown(f, f.area(), view);
        })
        .unwrap();
        let buf = term.backend().buffer().clone();
        let mut s = String::new();
        for y in 0..buf.area.height {
            for x in 0..buf.area.width {
                s.push_str(buf[(x, y)].symbol());
            }
            s.push('\n');
        }
        s
    }

    fn view(state: Fetch<Vec<LogLine>>, rev: usize) -> DrillView {
        DrillView {
            unit: "kairos-orderhub.service".to_string(),
            state,
            rev,
        }
    }

    #[test]
    fn renders_all_states_without_panic() {
        buffer_text(100, 30, &view(Fetch::Loading, 0));
        buffer_text(100, 30, &view(Fetch::Err("boom".to_string()), 0));
        buffer_text(100, 30, &view(Fetch::Ok(vec![]), 0));
        let many: Vec<_> = (0..500)
            .map(|i| log("2026-07-04T00:00:00", &format!("line {i}")))
            .collect();
        buffer_text(100, 30, &view(Fetch::Ok(many), 9_999));
    }

    #[test]
    fn header_names_the_unit() {
        let text = buffer_text(100, 30, &view(Fetch::Loading, 0));
        assert!(
            text.contains("journal: kairos-orderhub.service"),
            "header missing:\n{text}"
        );
    }

    #[test]
    fn empty_state_line_shown() {
        let text = buffer_text(100, 30, &view(Fetch::Ok(vec![]), 0));
        assert!(
            text.contains("no journal entries for kairos-orderhub.service"),
            "empty-state missing:\n{text}"
        );
    }

    #[test]
    fn err_shows_message_and_stale_badge() {
        let text = buffer_text(
            100,
            30,
            &view(Fetch::Err("journalctl exited".to_string()), 0),
        );
        assert!(text.contains("[stale]"), "stale badge missing:\n{text}");
        assert!(
            text.contains("journalctl exited"),
            "error text missing:\n{text}"
        );
    }

    #[test]
    fn scroll_top_pins_and_clamps() {
        assert_eq!(scroll_top(5, 10, 0), 0);
        assert_eq!(scroll_top(10, 10, 0), 0);
        assert_eq!(scroll_top(100, 10, 0), 90);
        assert_eq!(scroll_top(100, 10, 5), 85);
        assert_eq!(scroll_top(100, 10, 90), 0);
        assert_eq!(scroll_top(100, 10, 9_999), 0);
        assert_eq!(scroll_top(100, 0, 0), 0);
    }

    #[test]
    fn newest_line_at_bottom_at_rev_zero() {
        let logs: Vec<_> = (0..40)
            .map(|i| log("2026-07-04T00:00:00", &format!("entry-{i}")))
            .collect();
        let text = buffer_text(80, 12, &view(Fetch::Ok(logs), 0));
        assert!(text.contains("entry-39"), "newest line clipped:\n{text}");
    }

    fn drawn_rev(w: u16, h: u16, view: &DrillView) -> usize {
        let mut term = Terminal::new(TestBackend::new(w, h)).unwrap();
        let mut got = usize::MAX;
        term.draw(|f| got = render_drilldown(f, f.area(), view))
            .unwrap();
        got
    }

    #[test]
    fn render_clamps_overscroll_to_max_rev() {
        let logs: Vec<_> = (0..200)
            .map(|i| log("2026-07-04T00:00:00", &format!("entry-{i}")))
            .collect();
        // viewport = h - 2 = 28, so max_rev = 200 - 28 = 172.
        assert_eq!(drawn_rev(80, 30, &view(Fetch::Ok(logs), 9_999)), 172);
    }

    #[test]
    fn render_clamps_unscrollable_content_to_zero() {
        let logs: Vec<_> = (0..3)
            .map(|i| log("2026-07-04T00:00:00", &format!("entry-{i}")))
            .collect();
        // Content fits the viewport, so any accumulated rev collapses to 0.
        assert_eq!(drawn_rev(80, 30, &view(Fetch::Ok(logs), 50)), 0);
    }

    #[test]
    fn oldest_line_reachable_at_high_rev() {
        let logs: Vec<_> = (0..40)
            .map(|i| log("2026-07-04T00:00:00", &format!("entry-{i}")))
            .collect();
        let text = buffer_text(80, 12, &view(Fetch::Ok(logs), 9_999));
        assert!(text.contains("entry-0"), "oldest line clipped:\n{text}");
    }
}
