//! Offline dual-sidecar consistency check (D2 bring-up harness).
//!
//! Reads two KQR recordings (or one recording carrying two `source`s) and
//! compares same-symbol streams across sources: per-source seq gaps, cross-source
//! trade-price mismatches with their timestamp skew, and final top-of-book
//! mismatches. File-based and side-effect-free; no live feed.
//!
//! Price comparison is scale-aware (a mantissa+scale from one broker is compared
//! to another broker's mantissa+scale without floating point), per
//! `schema/NORMALIZATION.md`.

use std::collections::BTreeMap;

use crate::decode::{FeedEvent, decode_feed_event};
use crate::model::{Quote, Trade};
use crate::record::{RecordError, RecordReader};

/// A seq discontinuity within one `(source, symbol, epoch)`: `prev_seq` was seen,
/// then `next_seq` arrived without the intervening numbers. A benign session
/// rebuild shows up as an epoch bump instead (not reported here).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SeqGap {
    pub source: u16,
    pub symbol: String,
    pub epoch: u32,
    pub prev_seq: u64,
    pub next_seq: u64,
}

/// The N-th trade of a symbol differs between the two sources.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TradeMismatch {
    pub symbol: String,
    pub index: usize,
    pub a_source: u16,
    pub b_source: u16,
    pub a_price_mantissa: i64,
    pub a_price_scale: u8,
    pub b_price_mantissa: i64,
    pub b_price_scale: u8,
    pub a_volume: i64,
    pub b_volume: i64,
}

/// The two sources emitted a different number of trades for one shared symbol
/// (one dropped or never saw some prints). Reported instead of a per-index price
/// comparison, which would be misaligned and fabricate mismatches.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TradeCountMismatch {
    pub symbol: String,
    pub a_source: u16,
    pub b_source: u16,
    pub a_count: usize,
    pub b_count: usize,
}

/// Timestamp skew between the two sources for the N-th trade of a symbol
/// (`a_trade_ts_us - b_trade_ts_us`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TsDelta {
    pub symbol: String,
    pub index: usize,
    pub delta_us: i64,
}

/// Final top-of-book (best bid or ask) differs between the two sources.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BookMismatch {
    pub symbol: String,
    pub side: BookSide,
    pub a_source: u16,
    pub b_source: u16,
    pub a_price_mantissa: i64,
    pub a_price_scale: u8,
    pub b_price_mantissa: i64,
    pub b_price_scale: u8,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BookSide {
    Bid,
    Ask,
}

#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct CompareReport {
    pub seq_gaps: Vec<SeqGap>,
    pub trade_count_mismatches: Vec<TradeCountMismatch>,
    pub trade_mismatches: Vec<TradeMismatch>,
    pub ts_deltas: Vec<TsDelta>,
    pub book_mismatches: Vec<BookMismatch>,
}

impl CompareReport {
    pub fn is_clean(&self) -> bool {
        self.seq_gaps.is_empty()
            && self.trade_count_mismatches.is_empty()
            && self.trade_mismatches.is_empty()
            && self.book_mismatches.is_empty()
    }
}

/// Compare two mantissa+scale prices for equality without floating point:
/// `a_mantissa * 10^b_scale == b_mantissa * 10^a_scale`. Returns true when the
/// scaling would overflow i128 (treated as "cannot prove different").
fn price_eq(a_mantissa: i64, a_scale: u8, b_mantissa: i64, b_scale: u8) -> bool {
    let a = 10i128
        .checked_pow(b_scale as u32)
        .and_then(|p| (a_mantissa as i128).checked_mul(p));
    let b = 10i128
        .checked_pow(a_scale as u32)
        .and_then(|p| (b_mantissa as i128).checked_mul(p));
    match (a, b) {
        (Some(a), Some(b)) => a == b,
        _ => true,
    }
}

/// Decode a whole KQR buffer into its ordered feed events, skipping records that
/// are not routable feed events (control frames / unknown variants). A torn or
/// corrupt record surfaces as an error.
pub fn read_kqr_events(bytes: &[u8]) -> Result<Vec<FeedEvent>, RecordError> {
    let (_, reader) = RecordReader::open(bytes)?;
    let mut out = Vec::new();
    for rec in reader {
        let rec = rec?;
        if let Ok(ev) = decode_feed_event(&rec.payload) {
            out.push(ev);
        }
    }
    Ok(out)
}

fn event_source(ev: &FeedEvent) -> u16 {
    match ev {
        FeedEvent::Quote(q) => q.source,
        FeedEvent::Trade(t) => t.source,
    }
}

/// Per-source seq gaps: within one `(source, symbol)`, seq must increase by one
/// while the epoch holds; an epoch bump resets tracking (a benign rebuild).
pub fn find_seq_gaps(events: &[FeedEvent]) -> Vec<SeqGap> {
    let mut last: BTreeMap<(u16, String), (u32, u64)> = BTreeMap::new();
    let mut gaps = Vec::new();
    for ev in events {
        let (source, symbol, epoch, seq) = match ev {
            FeedEvent::Quote(q) => (q.source, &q.symbol, q.epoch, q.seq),
            FeedEvent::Trade(t) => (t.source, &t.symbol, t.epoch, t.seq),
        };
        if seq == 0 {
            continue; // legacy / no-seq record
        }
        let key = (source, symbol.clone());
        if let Some(&(prev_epoch, prev_seq)) = last.get(&key)
            && epoch == prev_epoch
            && seq > prev_seq.saturating_add(1)
        {
            gaps.push(SeqGap {
                source,
                symbol: symbol.clone(),
                epoch,
                prev_seq,
                next_seq: seq,
            });
        }
        last.insert(key, (epoch, seq));
    }
    gaps
}

fn trades_by_symbol(events: &[FeedEvent]) -> BTreeMap<String, Vec<Trade>> {
    let mut out: BTreeMap<String, Vec<Trade>> = BTreeMap::new();
    for ev in events {
        if let FeedEvent::Trade(t) = ev {
            out.entry(t.symbol.clone()).or_default().push(t.clone());
        }
    }
    out
}

fn last_quote_by_symbol(events: &[FeedEvent]) -> BTreeMap<String, Quote> {
    let mut out: BTreeMap<String, Quote> = BTreeMap::new();
    for ev in events {
        if let FeedEvent::Quote(q) = ev {
            out.insert(q.symbol.clone(), q.clone());
        }
    }
    out
}

/// Compare two decoded streams (`a`, `b`), each typically a single source.
/// Detects per-source seq gaps in both, plus cross-source trade-price mismatches
/// (with per-trade ts skew) and final top-of-book mismatches for shared symbols.
pub fn compare_streams(a: &[FeedEvent], b: &[FeedEvent]) -> CompareReport {
    let mut report = CompareReport::default();
    report.seq_gaps.extend(find_seq_gaps(a));
    report.seq_gaps.extend(find_seq_gaps(b));

    let a_src = a.first().map(event_source).unwrap_or(0);
    let b_src = b.first().map(event_source).unwrap_or(0);

    let a_trades = trades_by_symbol(a);
    let b_trades = trades_by_symbol(b);
    for (symbol, at) in &a_trades {
        let Some(bt) = b_trades.get(symbol) else {
            continue;
        };
        // Positional (Nth-vs-Nth) price comparison is only meaningful when both
        // sources carry the same run of trades. A count divergence (a dropped or
        // missed print) misaligns every following pair, so report the count and
        // skip the per-index comparison instead of fabricating mismatches.
        if at.len() != bt.len() {
            report.trade_count_mismatches.push(TradeCountMismatch {
                symbol: symbol.clone(),
                a_source: at.first().map(|t| t.source).unwrap_or(a_src),
                b_source: bt.first().map(|t| t.source).unwrap_or(b_src),
                a_count: at.len(),
                b_count: bt.len(),
            });
            continue;
        }
        for (i, (ta, tb)) in at.iter().zip(bt.iter()).enumerate() {
            if !price_eq(
                ta.price_mantissa,
                ta.price_scale,
                tb.price_mantissa,
                tb.price_scale,
            ) || ta.volume != tb.volume
            {
                report.trade_mismatches.push(TradeMismatch {
                    symbol: symbol.clone(),
                    index: i,
                    a_source: ta.source,
                    b_source: tb.source,
                    a_price_mantissa: ta.price_mantissa,
                    a_price_scale: ta.price_scale,
                    b_price_mantissa: tb.price_mantissa,
                    b_price_scale: tb.price_scale,
                    a_volume: ta.volume,
                    b_volume: tb.volume,
                });
            }
            report.ts_deltas.push(TsDelta {
                symbol: symbol.clone(),
                index: i,
                delta_us: ta.trade_ts_us - tb.trade_ts_us,
            });
        }
    }

    let a_books = last_quote_by_symbol(a);
    let b_books = last_quote_by_symbol(b);
    for (symbol, qa) in &a_books {
        let Some(qb) = b_books.get(symbol) else {
            continue;
        };
        for (side, la, lb) in [
            (BookSide::Bid, qa.bids.first(), qb.bids.first()),
            (BookSide::Ask, qa.asks.first(), qb.asks.first()),
        ] {
            if let (Some(la), Some(lb)) = (la, lb)
                && !price_eq(
                    la.price_mantissa,
                    la.price_scale,
                    lb.price_mantissa,
                    lb.price_scale,
                )
            {
                report.book_mismatches.push(BookMismatch {
                    symbol: symbol.clone(),
                    side,
                    a_source: a_src,
                    b_source: b_src,
                    a_price_mantissa: la.price_mantissa,
                    a_price_scale: la.price_scale,
                    b_price_mantissa: lb.price_mantissa,
                    b_price_scale: lb.price_scale,
                });
            }
        }
    }

    report
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::encode::{encode_quote, encode_trade};
    use crate::model::{Exchange, PriceLevel, Quote, QuoteBoard, Session, Trade};
    use crate::record::{FileHeader, RecordWriter};

    fn quote(source: u16, symbol: &str, seq: u64, epoch: u32, bid: i64, ask: i64) -> Quote {
        Quote {
            symbol: symbol.to_owned(),
            exchange: Exchange::Twse,
            quote_ts_us: 0,
            bids: vec![PriceLevel {
                price_mantissa: bid,
                price_scale: 2,
                volume: 1,
            }],
            asks: vec![PriceLevel {
                price_mantissa: ask,
                price_scale: 2,
                volume: 1,
            }],
            last_price: 0,
            last_scale: 0,
            last_volume: 0,
            is_trial: false,
            source,
            seq,
            epoch,
            recv_ts_us: 0,
            board: QuoteBoard::RoundLot,
            session: Session::Unknown,
            trading_date: 0,
            simtrade: false,
            underlying_price: 0,
        }
    }

    fn trade(
        source: u16,
        symbol: &str,
        seq: u64,
        epoch: u32,
        mant: i64,
        scale: u8,
        ts: i64,
    ) -> Trade {
        Trade {
            symbol: symbol.to_owned(),
            exchange: Exchange::Twse,
            source,
            seq,
            epoch,
            trade_ts_us: ts,
            recv_ts_us: 0,
            price_mantissa: mant,
            price_scale: scale,
            volume: 1,
            is_trial: false,
            session: Session::Unknown,
            trading_date: 0,
            simtrade: false,
            underlying_price: 0,
        }
    }

    fn to_kqr(events: &[FeedEvent]) -> Vec<u8> {
        let mut w = RecordWriter::create(Vec::new(), &FileHeader::new(1001, 0)).unwrap();
        for (i, ev) in events.iter().enumerate() {
            let bytes = match ev {
                FeedEvent::Quote(q) => encode_quote(q),
                FeedEvent::Trade(t) => encode_trade(t),
            };
            w.append(i as i64, &bytes).unwrap();
        }
        w.into_inner()
    }

    #[test]
    fn reads_kqr_events_back() {
        let events = vec![
            FeedEvent::Quote(quote(0, "2330", 1, 1, 58000, 58100)),
            FeedEvent::Trade(trade(0, "2330", 2, 1, 58050, 2, 10)),
        ];
        let buf = to_kqr(&events);
        let back = read_kqr_events(&buf).unwrap();
        assert_eq!(back, events);
    }

    #[test]
    fn detects_seq_gap_within_epoch() {
        let events = vec![
            FeedEvent::Quote(quote(0, "2330", 1, 1, 58000, 58100)),
            FeedEvent::Quote(quote(0, "2330", 4, 1, 58000, 58100)), // missing 2,3
        ];
        let gaps = find_seq_gaps(&events);
        assert_eq!(gaps.len(), 1);
        assert_eq!(gaps[0].prev_seq, 1);
        assert_eq!(gaps[0].next_seq, 4);
    }

    #[test]
    fn epoch_bump_is_not_a_gap() {
        let events = vec![
            FeedEvent::Quote(quote(0, "2330", 9, 1, 58000, 58100)),
            FeedEvent::Quote(quote(0, "2330", 1, 2, 58000, 58100)), // rebuild: epoch up, seq reset
        ];
        assert!(find_seq_gaps(&events).is_empty());
    }

    #[test]
    fn detects_trade_mismatch_and_ts_delta_scale_aware() {
        // Source 0 reports 580.50 as (58050, scale 2); source 1 as (5805, scale 1)
        // -> equal, no mismatch. A differing price on the 2nd trade is caught.
        let a = vec![
            FeedEvent::Trade(trade(0, "2330", 1, 1, 58050, 2, 1000)),
            FeedEvent::Trade(trade(0, "2330", 2, 1, 58100, 2, 2000)),
        ];
        let b = vec![
            FeedEvent::Trade(trade(1, "2330", 1, 1, 5805, 1, 900)),
            FeedEvent::Trade(trade(1, "2330", 2, 1, 58200, 2, 1950)),
        ];
        let report = compare_streams(&a, &b);
        assert_eq!(report.trade_mismatches.len(), 1);
        assert_eq!(report.trade_mismatches[0].index, 1);
        assert_eq!(report.ts_deltas.len(), 2);
        assert_eq!(report.ts_deltas[0].delta_us, 100);
        assert_eq!(report.ts_deltas[1].delta_us, 50);
    }

    #[test]
    fn detects_book_mismatch() {
        let a = vec![FeedEvent::Quote(quote(0, "2330", 1, 1, 58000, 58100))];
        let b = vec![FeedEvent::Quote(quote(1, "2330", 1, 1, 57990, 58100))];
        let report = compare_streams(&a, &b);
        assert_eq!(report.book_mismatches.len(), 1);
        assert_eq!(report.book_mismatches[0].side, BookSide::Bid);
        assert!(!report.is_clean());
    }

    #[test]
    fn clean_when_sources_agree() {
        let a = vec![
            FeedEvent::Quote(quote(0, "2330", 1, 1, 58000, 58100)),
            FeedEvent::Trade(trade(0, "2330", 2, 1, 58050, 2, 1000)),
        ];
        let b = vec![
            FeedEvent::Quote(quote(1, "2330", 1, 1, 58000, 58100)),
            FeedEvent::Trade(trade(1, "2330", 2, 1, 5805, 1, 1000)),
        ];
        let report = compare_streams(&a, &b);
        assert!(report.is_clean());
        assert_eq!(report.ts_deltas.len(), 1);
        assert_eq!(report.ts_deltas[0].delta_us, 0);
    }
}
