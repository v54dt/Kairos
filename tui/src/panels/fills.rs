use ratatui::Frame;
use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph};

use crate::format::format_price;
use crate::panels::listview;
use crate::sources::order_journal::{self, FeeParams, Fill, Settlement};

const SETTLE_HEIGHT: u16 = 6;
const SYMBOL_WIDTH: usize = 10;

fn dim(text: &str) -> Line<'static> {
    Line::from(Span::styled(
        text.to_string(),
        Style::default().fg(Color::DarkGray),
    ))
}

/// `YYYYMMDD` -> `YYYY-MM-DD` for the panel title; anything else passes through.
fn date_label(date: &str) -> String {
    if date.len() == 8 && date.chars().all(|c| c.is_ascii_digit()) {
        format!("{}-{}-{}", &date[0..4], &date[4..6], &date[6..8])
    } else {
        date.to_string()
    }
}

/// `HH:MM:SS` in Taipei time (UTC+8) from a micros timestamp.
fn hms_tw(t_us: i64) -> String {
    let secs = t_us.div_euclid(1_000_000) + 8 * 3600;
    let sod = secs.rem_euclid(86_400);
    format!("{:02}:{:02}:{:02}", sod / 3600, (sod % 3600) / 60, sod % 60)
}

/// Group a non-negative integer into comma-separated thousands.
fn commas(n: u128) -> String {
    let digits = n.to_string();
    let len = digits.len();
    let mut out = String::with_capacity(len + len / 3);
    for (i, ch) in digits.chars().enumerate() {
        if i > 0 && (len - i).is_multiple_of(3) {
            out.push(',');
        }
        out.push(ch);
    }
    out
}

/// A signed whole-TWD amount (from cents) with an explicit +/- sign.
fn money_signed(cents: i128) -> String {
    let twd = cents / 100;
    let sign = if twd < 0 { "-" } else { "+" };
    format!("{sign}{}", commas(twd.unsigned_abs()))
}

/// An unsigned whole-TWD amount (from cents), for fee/tax components.
fn money_abs(cents: i128) -> String {
    commas((cents / 100).unsigned_abs())
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
            "{:<8}  {:<w$} {:<5} {:>9} {:>10}",
            "time",
            "scenario",
            "side",
            "shares",
            "price",
            w = SYMBOL_WIDTH,
        ),
        Style::default().fg(Color::DarkGray),
    ))
}

fn row_line(f: &Fill, selected: bool) -> Line<'static> {
    let (side, color) = if f.buy {
        ("BUY", Color::Red)
    } else {
        ("SELL", Color::Green)
    };
    let mut line = Line::from(vec![
        Span::raw(format!("{:<8}  ", hms_tw(f.t))),
        Span::raw(format!(
            "{:<w$} ",
            trunc(&f.symbol(), SYMBOL_WIDTH),
            w = SYMBOL_WIDTH
        )),
        Span::styled(format!("{side:<5}"), Style::default().fg(color)),
        Span::raw(format!(" {:>9}", commas(f.shares as u128))),
        Span::raw(format!(" {:>10}", format_price(f.price, 2))),
    ]);
    if selected {
        line = line.style(Style::default().add_modifier(Modifier::REVERSED));
    }
    line
}

fn data_lines(fills: &[Fill], sel: usize) -> Vec<Line<'static>> {
    if fills.is_empty() {
        return vec![dim("no fills for today")];
    }
    fills
        .iter()
        .enumerate()
        .map(|(i, f)| row_line(f, i == sel))
        .collect()
}

fn settlement_lines(s: &Settlement) -> Vec<Line<'static>> {
    let red = Style::default().fg(Color::Red);
    let green = Style::default().fg(Color::Green);
    let net_style = if s.net_cents() < 0 { red } else { green };
    vec![
        Line::from(vec![
            Span::raw(format!("{:<12}", "買進 交割應付")),
            Span::styled(format!("{:>14}", money_signed(s.payable_cents())), red),
            Span::raw(format!("   (fee {})", money_abs(s.buy_fee_cents))),
        ]),
        Line::from(vec![
            Span::raw(format!("{:<12}", "賣出 交割應收")),
            Span::styled(format!("{:>14}", money_signed(s.receivable_cents())), green),
            Span::raw(format!(
                "   (fee {}  tax {})",
                money_abs(s.sell_fee_cents),
                money_abs(s.sell_tax_cents)
            )),
        ]),
        Line::from(vec![
            Span::styled(
                format!("{:<12}", "淨交割金額"),
                Style::default().add_modifier(Modifier::BOLD),
            ),
            Span::styled(
                format!("{:>14}", money_signed(s.net_cents())),
                net_style.add_modifier(Modifier::BOLD),
            ),
            Span::raw("   (T+2)"),
        ]),
        dim(
            "est. fee 0.1425%\u{00d7}discount (min 1 TWD odd-lot / 20 TWD round-lot), \
             sell tax 0.3% (assumes non-day-trade), T+2 net",
        ),
    ]
}

pub fn render(frame: &mut Frame, area: Rect, fills: &[Fill], date: &str, sel: usize) {
    let rows = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Min(0), Constraint::Length(SETTLE_HEIGHT)])
        .split(area);

    let sel = sel.min(fills.len().saturating_sub(1));
    let title = format!(
        "today's fills ({})  {} fills",
        date_label(date),
        fills.len()
    );
    let list_block = Block::default().title(title).borders(Borders::ALL);
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
    let offset = listview::scroll_offset(view_h, sel);
    frame.render_widget(
        Paragraph::new(data_lines(fills, sel)).scroll((offset, 0)),
        parts[1],
    );
    frame.render_widget(
        Paragraph::new(listview::footer_line(fills.len(), offset as usize, view_h)),
        parts[2],
    );

    let settlement = order_journal::settle(fills, &FeeParams::default());
    frame.render_widget(
        Paragraph::new(settlement_lines(&settlement)).block(
            Block::default()
                .title("settlement (est.)")
                .borders(Borders::ALL),
        ),
        rows[1],
    );
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::panels::test_util::buffer_text;

    fn draw_fills<'a>(
        fills: &'a [Fill],
        date: &'a str,
        sel: usize,
    ) -> impl FnOnce(&mut ratatui::Frame) + 'a {
        move |f| render(f, f.area(), fills, date, sel)
    }

    fn buy(t: i64, shares: i64, price: i64) -> Fill {
        Fill {
            t,
            stem: "2330-Buy-20260705".to_string(),
            buy: true,
            shares,
            price,
        }
    }

    fn sell(t: i64, shares: i64, price: i64) -> Fill {
        Fill {
            t,
            stem: "2327-Sell-20260705".to_string(),
            buy: false,
            shares,
            price,
        }
    }

    #[test]
    fn renders_empty_without_panic() {
        buffer_text(100, 30, draw_fills(&[], "20260705", 0));
    }

    #[test]
    fn renders_header_rows_footer_and_money_lines() {
        // 09:04:12 Taipei = (9*3600+4*60+12) - 8*3600 seconds UTC.
        let t_buy = (9 * 3600 + 4 * 60 + 12 - 8 * 3600) as i64 * 1_000_000;
        let t_sell = (9 * 3600 + 5 * 60 + 20 - 8 * 3600) as i64 * 1_000_000;
        let fills = vec![buy(t_buy, 3000, 58500), sell(t_sell, 1000, 41250)];
        let text = buffer_text(100, 30, draw_fills(&fills, "20260705", 0));
        assert!(
            text.contains("today's fills (2026-07-05)"),
            "title:\n{text}"
        );
        assert!(text.contains("2 fills"), "count:\n{text}");
        assert!(text.contains("time"), "detail header:\n{text}");
        assert!(text.contains("09:04:12"), "BUY row time:\n{text}");
        assert!(text.contains("BUY"), "BUY row side:\n{text}");
        assert!(text.contains("3,000"), "BUY row shares:\n{text}");
        assert!(text.contains("585.00"), "BUY row price:\n{text}");
        assert!(text.contains("09:05:20"), "SELL row time:\n{text}");
        assert!(text.contains("SELL"), "SELL row side:\n{text}");
        assert!(text.contains("showing 1\u{2013}2 of 2"), "footer:\n{text}");
        // Wide CJK cells get a trailing skip cell in the test buffer, so match
        // the labels by a distinctive character plus the line's ASCII marker.
        assert!(text.contains('買'), "payable label:\n{text}");
        assert!(text.contains('賣'), "receivable label:\n{text}");
        assert!(text.contains('淨'), "net label:\n{text}");
        assert!(text.contains("(T+2)"), "net T+2 marker:\n{text}");
        // -(1,755,000 + 2,500) = -1,757,500, with (fee 2,500).
        assert!(text.contains("-1,757,500"), "payable amount:\n{text}");
        assert!(text.contains("(fee 2,500)"), "buy fee component:\n{text}");
        // 412,500 - 587 - 1,237 = 410,676, with (fee 587  tax 1,237).
        assert!(text.contains("+410,676"), "receivable amount:\n{text}");
        assert!(
            text.contains("(fee 587  tax 1,237)"),
            "sell fee/tax:\n{text}"
        );
        // net -1,346,824.
        assert!(text.contains("-1,346,824"), "net amount:\n{text}");
    }

    #[test]
    fn footer_shows_range_of_total_for_many_fills() {
        let fills: Vec<Fill> = (0..60).map(|i| buy(i, 1000, 58500)).collect();
        let text = buffer_text(100, 20, draw_fills(&fills, "20260705", 0));
        assert!(text.contains("of 60"), "total:\n{text}");
        assert!(text.contains("60 fills"), "count:\n{text}");
    }

    #[test]
    fn settlement_footnote_states_lot_floors_and_non_daytrade() {
        let lines = settlement_lines(&Settlement::default());
        let footnote: String = lines
            .last()
            .unwrap()
            .spans
            .iter()
            .map(|s| s.content.as_ref())
            .collect();
        assert_eq!(
            footnote,
            "est. fee 0.1425%\u{00d7}discount (min 1 TWD odd-lot / 20 TWD round-lot), \
             sell tax 0.3% (assumes non-day-trade), T+2 net"
        );
    }

    #[test]
    fn commas_group_thousands() {
        assert_eq!(commas(0), "0");
        assert_eq!(commas(999), "999");
        assert_eq!(commas(1_000), "1,000");
        assert_eq!(commas(1_757_501), "1,757,501");
    }

    #[test]
    fn money_signed_shows_sign() {
        assert_eq!(money_signed(-175_750_100), "-1,757,501");
        assert_eq!(money_signed(41_067_600), "+410,676");
    }

    // End-to-end: hand-written journal files for today, scanned and rendered.
    #[test]
    fn scan_then_render_matches_hand_calc() {
        use crate::sources::order_journal::{scan_fills, today_tw};
        let date = today_tw();
        let dir = std::env::temp_dir().join(format!("kairos-panel-fills-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(
            dir.join(format!("2330-Buy-{date}.jsonl")),
            "{\"t\":10,\"type\":\"fill\",\"id\":\"a\",\"shares\":3000,\"price\":58500}\n",
        )
        .unwrap();
        std::fs::write(
            dir.join(format!("2327-Sell-{date}.jsonl")),
            "{\"t\":20,\"type\":\"fill\",\"id\":\"b\",\"shares\":-1000,\"price\":41250}\n",
        )
        .unwrap();
        let fills = scan_fills(&dir, &date);
        std::fs::remove_dir_all(&dir).ok();
        let text = buffer_text(100, 30, draw_fills(&fills, &date, 0));
        assert!(text.contains("2 fills"), "count:\n{text}");
        assert!(text.contains("-1,757,500"), "payable:\n{text}");
        assert!(text.contains("+410,676"), "receivable:\n{text}");
        assert!(text.contains("-1,346,824"), "net:\n{text}");
    }
}
