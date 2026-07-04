use ratatui::Frame;
use ratatui::layout::Rect;
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph};

use crate::app::{Config, Fetch};
use crate::panels::{error_line, human_bytes, loading_line};
use crate::sources::recorder::RecorderStats;

const RED_FREE: u64 = 1 << 30;
const YELLOW_FREE: u64 = 5 * (1 << 30);

fn disk_line(free: Option<u64>, cfg: &Config) -> Line<'static> {
    match free {
        None => Line::from(vec![
            Span::raw("disk free  "),
            Span::styled(
                format!("n/a ({})", cfg.data_dir.display()),
                Style::default().fg(Color::DarkGray),
            ),
        ]),
        Some(bytes) => {
            let color = if bytes < RED_FREE {
                Color::Red
            } else if bytes < YELLOW_FREE {
                Color::Yellow
            } else {
                Color::Green
            };
            Line::from(vec![
                Span::raw("disk free  "),
                Span::styled(
                    human_bytes(bytes),
                    Style::default().fg(color).add_modifier(Modifier::BOLD),
                ),
            ])
        }
    }
}

fn stats_lines(stats: &RecorderStats) -> Vec<Line<'static>> {
    let drop_color = if stats.drops > 0 || stats.write_errs > 0 {
        Color::Red
    } else {
        Color::Green
    };
    let stream_style = if stats.drops > 0 || stats.write_errs > 0 {
        Style::default().fg(Color::Red).add_modifier(Modifier::BOLD)
    } else {
        Style::default()
    };
    vec![
        Line::from(vec![
            Span::raw("stream     "),
            Span::styled(stats.stream.to_string(), stream_style),
        ]),
        Line::from(format!("  records    {}", stats.records)),
        Line::from(format!("  bytes      {}", stats.bytes)),
        Line::from(vec![
            Span::raw("  drops      "),
            Span::styled(stats.drops.to_string(), Style::default().fg(drop_color)),
            Span::raw(format!("   write_errs {}", stats.write_errs)),
        ]),
    ]
}

pub fn render(
    frame: &mut Frame,
    area: Rect,
    state: &Fetch<Vec<RecorderStats>>,
    disk_free: Option<u64>,
    cfg: &Config,
) {
    let block = Block::default().title("recorder").borders(Borders::ALL);
    let mut lines: Vec<Line> = match state {
        Fetch::Loading => vec![loading_line()],
        Fetch::Err(e) => vec![error_line(e)],
        Fetch::Ok(stats) if stats.is_empty() => vec![Line::from(Span::styled(
            "no stats line",
            Style::default().fg(Color::DarkGray),
        ))],
        Fetch::Ok(stats) => stats.iter().flat_map(stats_lines).collect(),
    };
    lines.push(disk_line(disk_free, cfg));
    frame.render_widget(Paragraph::new(lines).block(block), area);
}
