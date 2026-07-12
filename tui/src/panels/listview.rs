use ratatui::style::{Color, Style};
use ratatui::text::{Line, Span};

/// Vertical scroll so the 0-indexed selected data row stays visible inside a
/// `view_h`-tall list area (header/border rendered separately, so no adjustment
/// here).
pub(super) fn scroll_offset(view_h: usize, sel: usize) -> u16 {
    if view_h > 0 && sel >= view_h {
        (sel - view_h + 1) as u16
    } else {
        0
    }
}

/// The dim "showing a\u{2013}b of n" footer for a windowed list.
pub(super) fn footer_line(total: usize, offset: usize, view_h: usize) -> Line<'static> {
    let text = if total == 0 {
        "showing 0\u{2013}0 of 0".to_string()
    } else {
        let first = offset + 1;
        let last = (offset + view_h.max(1)).min(total);
        format!("showing {first}\u{2013}{last} of {total}")
    };
    Line::from(Span::styled(text, Style::default().fg(Color::DarkGray)))
}
