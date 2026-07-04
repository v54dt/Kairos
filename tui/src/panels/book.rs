use std::time::Instant;

use ratatui::Frame;
use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph};

use crate::app::Config;
use crate::format::format_price;
use crate::sources::age::format_age;
use crate::sources::feed::{FeedState, SymbolBook};

const DEPTH: usize = 5;

pub fn render(frame: &mut Frame, area: Rect, state: &FeedState, cfg: &Config) {
    if cfg.symbols.is_empty() {
        let block = Block::default()
            .title("Feeds & Books")
            .borders(Borders::ALL);
        frame.render_widget(
            Paragraph::new(Line::from(Span::styled(
                "no symbols configured",
                Style::default().fg(Color::DarkGray),
            )))
            .block(block),
            area,
        );
        return;
    }

    let n = cfg.symbols.len() as u32;
    let constraints: Vec<Constraint> = cfg
        .symbols
        .iter()
        .map(|_| Constraint::Ratio(1, n))
        .collect();
    let cols = Layout::default()
        .direction(Direction::Horizontal)
        .constraints(constraints)
        .split(area);

    let now = Instant::now();
    for (i, symbol) in cfg.symbols.iter().enumerate() {
        let book = state.per_symbol.get(symbol);
        render_symbol(frame, cols[i], symbol, book, state.connected, now);
    }
}

fn render_symbol(
    frame: &mut Frame,
    area: Rect,
    symbol: &str,
    book: Option<&SymbolBook>,
    connected: bool,
    now: Instant,
) {
    let block = Block::default()
        .title(format!(" {symbol} "))
        .borders(Borders::ALL);

    let mut lines: Vec<Line> = Vec::new();

    match book {
        None => {
            if connected {
                lines.push(dim("waiting for data..."));
            } else {
                lines.push(disconnected_badge());
                lines.push(dim("no data"));
            }
        }
        Some(b) => {
            let mut header = vec![status_span(connected), Span::raw("  ")];
            header.push(Span::styled(
                format!("src {}", b.source),
                Style::default().fg(Color::DarkGray),
            ));
            if b.is_trial {
                header.push(Span::raw(" "));
                header.push(trial_badge());
            }
            lines.push(Line::from(header));

            let age = b
                .last_seen
                .map(|t| now.saturating_duration_since(t))
                .map(format_age)
                .unwrap_or_else(|| "-".to_string());
            lines.push(dim(&format!("age {age}")));
            lines.push(Line::raw(""));

            let base = level_style(b.is_trial);
            for row in (0..DEPTH).rev() {
                lines.push(level_line(
                    "A",
                    b.asks.get(row),
                    base.fg(Color::Red),
                    b.is_trial,
                ));
            }
            lines.push(Line::from(Span::styled(
                "  ----------------",
                Style::default().fg(Color::DarkGray),
            )));
            for row in 0..DEPTH {
                lines.push(level_line(
                    "B",
                    b.bids.get(row),
                    base.fg(Color::Green),
                    b.is_trial,
                ));
            }

            lines.push(Line::raw(""));
            let last = format_price(b.last_price, b.last_scale);
            lines.push(Line::from(vec![
                Span::raw("last "),
                Span::styled(
                    format!("{last:>10}"),
                    level_style(b.is_trial).add_modifier(Modifier::BOLD),
                ),
                Span::styled(
                    format!(" x{}", b.last_volume),
                    Style::default().fg(Color::DarkGray),
                ),
            ]));
        }
    }

    frame.render_widget(Paragraph::new(lines).block(block), area);
}

fn level_line(
    tag: &str,
    level: Option<&kairos_core::model::PriceLevel>,
    style: Style,
    trial: bool,
) -> Line<'static> {
    match level {
        Some(l) => {
            let price = format_price(l.price_mantissa, l.price_scale);
            Line::from(vec![
                Span::styled(format!("{tag} "), Style::default().fg(Color::DarkGray)),
                Span::styled(format!("{price:>10}"), style),
                Span::styled(
                    format!("  {:>7}", l.volume),
                    dim_style(trial).fg(Color::Gray),
                ),
            ])
        }
        None => Line::from(Span::styled(
            format!("{tag} {:>10}  {:>7}", "-", "-"),
            Style::default().fg(Color::DarkGray),
        )),
    }
}

fn status_span(connected: bool) -> Span<'static> {
    if connected {
        Span::styled(
            "CONNECTED",
            Style::default()
                .fg(Color::Green)
                .add_modifier(Modifier::BOLD),
        )
    } else {
        disconnected_span()
    }
}

fn disconnected_badge() -> Line<'static> {
    Line::from(disconnected_span())
}

fn disconnected_span() -> Span<'static> {
    Span::styled(
        "DISCONNECTED",
        Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
    )
}

fn trial_badge() -> Span<'static> {
    Span::styled(
        "TRIAL",
        Style::default()
            .fg(Color::Yellow)
            .add_modifier(Modifier::DIM),
    )
}

fn level_style(trial: bool) -> Style {
    dim_style(trial)
}

fn dim_style(trial: bool) -> Style {
    if trial {
        Style::default().add_modifier(Modifier::DIM)
    } else {
        Style::default()
    }
}

fn dim(text: &str) -> Line<'static> {
    Line::from(Span::styled(
        text.to_string(),
        Style::default().fg(Color::DarkGray),
    ))
}
