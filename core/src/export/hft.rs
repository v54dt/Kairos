//! hftbacktest `_v1` event mapping: turns decoded Quotes/Trades into the 64-byte
//! structured events nkaz001/hftbacktest v2 ingests, buffered per symbol and
//! written as one `.npy` per (symbol, day). See the module-level doc in
//! `export::mod` for the KNOWN-vs-ASSUMPTION layout contract.

use std::collections::BTreeMap;
use std::io::{self, Write};

use super::npy::write_npy;
use crate::model::{Quote, Trade};

/// The structured dtype `descr` for a 64-byte hftbacktest event (8 x 8-byte LE
/// fields). Kept in sync with the golden header in `npy` tests.
pub const EVENT_DESCR: &str = "[('ev', '<u8'), ('exch_ts', '<i8'), ('local_ts', '<i8'), \
     ('px', '<f8'), ('qty', '<f8'), ('order_id', '<u8'), ('ival', '<i8'), ('fval', '<f8')]";

// Event-type low bits.
pub const DEPTH_EVENT: u64 = 1;
pub const TRADE_EVENT: u64 = 2;
pub const DEPTH_CLEAR_EVENT: u64 = 3;
pub const DEPTH_SNAPSHOT_EVENT: u64 = 4;
// Flag high bits, OR'd onto the event type.
pub const EXCH_EVENT: u64 = 1 << 31;
pub const LOCAL_EVENT: u64 = 1 << 30;
pub const BUY_EVENT: u64 = 1 << 29;
pub const SELL_EVENT: u64 = 1 << 28;

const NS_PER_US: i64 = 1000;

/// One 64-byte hftbacktest event.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Event {
    pub ev: u64,
    pub exch_ts: i64,
    pub local_ts: i64,
    pub px: f64,
    pub qty: f64,
    pub order_id: u64,
    pub ival: i64,
    pub fval: f64,
}

impl Event {
    pub const SIZE: usize = 64;

    pub fn to_le_bytes(&self) -> [u8; Self::SIZE] {
        let mut b = [0u8; Self::SIZE];
        b[0..8].copy_from_slice(&self.ev.to_le_bytes());
        b[8..16].copy_from_slice(&self.exch_ts.to_le_bytes());
        b[16..24].copy_from_slice(&self.local_ts.to_le_bytes());
        b[24..32].copy_from_slice(&self.px.to_le_bytes());
        b[32..40].copy_from_slice(&self.qty.to_le_bytes());
        b[40..48].copy_from_slice(&self.order_id.to_le_bytes());
        b[48..56].copy_from_slice(&self.ival.to_le_bytes());
        b[56..64].copy_from_slice(&self.fval.to_le_bytes());
        b
    }
}

fn px_of(mantissa: i64, scale: u8) -> f64 {
    mantissa as f64 / 10f64.powi(scale as i32)
}

fn depth_row(ev: u64, exch_ts: i64, local_ts: i64, px: f64, qty: f64) -> Event {
    Event {
        ev,
        exch_ts,
        local_ts,
        px,
        qty,
        order_id: 0,
        ival: 0,
        fval: 0.0,
    }
}

/// Expand a Quote into a clear+snapshot block per side. Padding levels with zero
/// price or zero volume are skipped.
pub fn quote_to_events(q: &Quote, frame_recv_ts_us: i64, out: &mut Vec<Event>) {
    let exch_ts = q.quote_ts_us.saturating_mul(NS_PER_US);
    let local_ts = frame_recv_ts_us.saturating_mul(NS_PER_US);

    out.push(depth_row(
        DEPTH_CLEAR_EVENT | EXCH_EVENT | LOCAL_EVENT | BUY_EVENT,
        exch_ts,
        local_ts,
        0.0,
        0.0,
    ));
    for lvl in &q.bids {
        if lvl.price_mantissa == 0 || lvl.volume == 0 {
            continue;
        }
        out.push(depth_row(
            DEPTH_SNAPSHOT_EVENT | EXCH_EVENT | LOCAL_EVENT | BUY_EVENT,
            exch_ts,
            local_ts,
            px_of(lvl.price_mantissa, lvl.price_scale),
            lvl.volume as f64,
        ));
    }

    out.push(depth_row(
        DEPTH_CLEAR_EVENT | EXCH_EVENT | LOCAL_EVENT | SELL_EVENT,
        exch_ts,
        local_ts,
        0.0,
        0.0,
    ));
    for lvl in &q.asks {
        if lvl.price_mantissa == 0 || lvl.volume == 0 {
            continue;
        }
        out.push(depth_row(
            DEPTH_SNAPSHOT_EVENT | EXCH_EVENT | LOCAL_EVENT | SELL_EVENT,
            exch_ts,
            local_ts,
            px_of(lvl.price_mantissa, lvl.price_scale),
            lvl.volume as f64,
        ));
    }
}

/// Map a Trade to a single trade event. The wire carries no aggressor side, so no
/// BUY/SELL flag is set.
pub fn trade_to_event(t: &Trade, frame_recv_ts_us: i64) -> Event {
    Event {
        ev: TRADE_EVENT | EXCH_EVENT | LOCAL_EVENT,
        exch_ts: t.trade_ts_us.saturating_mul(NS_PER_US),
        local_ts: frame_recv_ts_us.saturating_mul(NS_PER_US),
        px: px_of(t.price_mantissa, t.price_scale),
        qty: t.volume as f64,
        order_id: 0,
        ival: 0,
        fval: 0.0,
    }
}

/// Total order for a per-symbol event stream. `local_ts` is primary so the feed
/// stays monotonic on hftbacktest's local clock; the rest keep a re-run
/// byte-identical.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
struct SortKey {
    local_ts: i64,
    exch_ts: i64,
    seq: u64,
    idx: u64,
}

#[derive(Default)]
struct SymbolBuf {
    events: Vec<(SortKey, Event)>,
}

/// Buffers events per symbol until end-of-input (an `.npy` needs its row count up
/// front), then yields each symbol's stream in a deterministic total order.
#[derive(Default)]
pub struct HftAccumulator {
    symbols: BTreeMap<String, SymbolBuf>,
    next_idx: u64,
}

impl HftAccumulator {
    pub fn new() -> Self {
        Self::default()
    }

    fn push(&mut self, symbol: &str, seq: u64, evs: Vec<Event>) {
        let keyed: Vec<(SortKey, Event)> = evs
            .into_iter()
            .map(|e| {
                let idx = self.next_idx;
                self.next_idx += 1;
                (
                    SortKey {
                        exch_ts: e.exch_ts,
                        local_ts: e.local_ts,
                        seq,
                        idx,
                    },
                    e,
                )
            })
            .collect();
        self.symbols
            .entry(symbol.to_owned())
            .or_default()
            .events
            .extend(keyed);
    }

    pub fn add_quote(&mut self, q: &Quote, frame_recv_ts_us: i64) {
        let mut evs = Vec::new();
        quote_to_events(q, frame_recv_ts_us, &mut evs);
        self.push(&q.symbol, q.seq, evs);
    }

    pub fn add_trade(&mut self, t: &Trade, frame_recv_ts_us: i64) {
        let ev = trade_to_event(t, frame_recv_ts_us);
        self.push(&t.symbol, t.seq, vec![ev]);
    }

    /// Consume the accumulator, yielding `(symbol, sorted events)` in ascending
    /// symbol order.
    pub fn into_sorted(self) -> Vec<(String, Vec<Event>)> {
        self.symbols
            .into_iter()
            .map(|(sym, mut buf)| {
                buf.events.sort_by(|a, b| a.0.cmp(&b.0));
                (sym, buf.events.into_iter().map(|(_, e)| e).collect())
            })
            .collect()
    }
}

/// Serialize a sorted event stream to its raw little-endian buffer.
pub fn events_to_bytes(events: &[Event]) -> Vec<u8> {
    let mut buf = Vec::with_capacity(events.len() * Event::SIZE);
    for e in events {
        buf.extend_from_slice(&e.to_le_bytes());
    }
    buf
}

/// Write one symbol's events as a `.npy` structured array.
pub fn write_symbol_npy<W: Write>(w: &mut W, events: &[Event]) -> io::Result<()> {
    let data = events_to_bytes(events);
    write_npy(w, EVENT_DESCR, events.len(), &data)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{Exchange, PriceLevel, Quote, Session, Trade};

    fn quote() -> Quote {
        Quote {
            symbol: "2330".to_owned(),
            exchange: Exchange::Twse,
            quote_ts_us: 1_700_000_000_000_000,
            bids: vec![
                PriceLevel {
                    price_mantissa: 58000,
                    price_scale: 2,
                    volume: 100,
                },
                PriceLevel {
                    price_mantissa: 0,
                    price_scale: 2,
                    volume: 0,
                },
            ],
            asks: vec![PriceLevel {
                price_mantissa: 58100,
                price_scale: 2,
                volume: 80,
            }],
            last_price: 58050,
            last_scale: 2,
            last_volume: 10,
            is_trial: false,
            source: 0,
            seq: 7,
            epoch: 1,
            recv_ts_us: 0,
            board: crate::model::QuoteBoard::RoundLot,
            session: Session::Day,
            trading_date: 20_260_704,
            simtrade: false,
            underlying_price: 0,
        }
    }

    #[test]
    fn quote_expands_to_clear_and_snapshot_rows() {
        let mut evs = Vec::new();
        quote_to_events(&quote(), 1_700_000_000_000_009, &mut evs);
        // clear bid, 1 populated bid (padding level skipped), clear ask, 1 ask.
        assert_eq!(evs.len(), 4);

        assert_eq!(
            evs[0].ev,
            DEPTH_CLEAR_EVENT | EXCH_EVENT | LOCAL_EVENT | BUY_EVENT
        );
        assert_eq!(evs[0].px, 0.0);
        assert_eq!(evs[0].exch_ts, 1_700_000_000_000_000 * 1000);
        assert_eq!(evs[0].local_ts, 1_700_000_000_000_009 * 1000);

        assert_eq!(
            evs[1].ev,
            DEPTH_SNAPSHOT_EVENT | EXCH_EVENT | LOCAL_EVENT | BUY_EVENT
        );
        assert_eq!(evs[1].px, 580.0);
        assert_eq!(evs[1].qty, 100.0);

        assert_eq!(
            evs[2].ev,
            DEPTH_CLEAR_EVENT | EXCH_EVENT | LOCAL_EVENT | SELL_EVENT
        );
        assert_eq!(
            evs[3].ev,
            DEPTH_SNAPSHOT_EVENT | EXCH_EVENT | LOCAL_EVENT | SELL_EVENT
        );
        assert_eq!(evs[3].px, 581.0);
        assert_eq!(evs[3].qty, 80.0);
    }

    #[test]
    fn exact_flag_words() {
        assert_eq!(EXCH_EVENT, 0x8000_0000);
        assert_eq!(LOCAL_EVENT, 0x4000_0000);
        assert_eq!(BUY_EVENT, 0x2000_0000);
        assert_eq!(SELL_EVENT, 0x1000_0000);
        assert_eq!(
            DEPTH_SNAPSHOT_EVENT | EXCH_EVENT | LOCAL_EVENT | BUY_EVENT,
            0xE000_0004
        );
    }

    #[test]
    fn trade_maps_to_single_trade_event() {
        let t = Trade {
            symbol: "2317".to_owned(),
            exchange: Exchange::Twse,
            source: 0,
            seq: 3,
            epoch: 1,
            trade_ts_us: 1_700_000_000_000_100,
            recv_ts_us: 0,
            price_mantissa: 11050,
            price_scale: 2,
            volume: 5,
            is_trial: false,
            session: Session::Day,
            trading_date: 20_260_704,
            simtrade: false,
            underlying_price: 0,
        };
        let e = trade_to_event(&t, 1_700_000_000_000_101);
        assert_eq!(e.ev, TRADE_EVENT | EXCH_EVENT | LOCAL_EVENT);
        assert_eq!(e.exch_ts, 1_700_000_000_000_100 * 1000);
        assert_eq!(e.local_ts, 1_700_000_000_000_101 * 1000);
        assert_eq!(e.px, 110.5);
        assert_eq!(e.qty, 5.0);
    }

    #[test]
    fn event_byte_layout() {
        let e = Event {
            ev: 0xE000_0004,
            exch_ts: 2,
            local_ts: 3,
            px: 580.0,
            qty: 100.0,
            order_id: 0,
            ival: 0,
            fval: 0.0,
        };
        let b = e.to_le_bytes();
        assert_eq!(b.len(), 64);
        assert_eq!(u64::from_le_bytes(b[0..8].try_into().unwrap()), 0xE000_0004);
        assert_eq!(i64::from_le_bytes(b[8..16].try_into().unwrap()), 2);
        assert_eq!(i64::from_le_bytes(b[16..24].try_into().unwrap()), 3);
        assert_eq!(f64::from_le_bytes(b[24..32].try_into().unwrap()), 580.0);
        assert_eq!(f64::from_le_bytes(b[32..40].try_into().unwrap()), 100.0);
    }

    #[test]
    fn local_ts_stays_monotonic_when_exch_ts_out_of_order() {
        let lvl = PriceLevel {
            price_mantissa: 100,
            price_scale: 2,
            volume: 1,
        };
        let mk = |quote_ts_us: i64| {
            let mut q = quote();
            q.quote_ts_us = quote_ts_us;
            q.bids = vec![lvl];
            q.asks = vec![];
            q
        };
        let mut acc = HftAccumulator::new();
        // Received in local order (200, 210) but with descending exch_ts (100, 90).
        acc.add_quote(&mk(100), 200);
        acc.add_quote(&mk(90), 210);
        let sorted = acc.into_sorted();
        let locals: Vec<i64> = sorted[0].1.iter().map(|e| e.local_ts).collect();
        assert!(locals.windows(2).all(|w| w[0] <= w[1]), "{locals:?}");
    }

    #[test]
    fn accumulator_is_deterministic() {
        let build = || {
            let mut acc = HftAccumulator::new();
            acc.add_quote(&quote(), 1_700_000_000_000_009);
            acc.add_quote(&quote(), 1_700_000_000_000_010);
            let sorted = acc.into_sorted();
            let (sym, evs) = &sorted[0];
            assert_eq!(sym, "2330");
            events_to_bytes(evs)
        };
        assert_eq!(build(), build());
        // Two quotes each expand to 4 rows.
        assert_eq!(build().len(), 8 * Event::SIZE);
    }
}
