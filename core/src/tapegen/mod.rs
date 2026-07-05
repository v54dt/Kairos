//! Synthetic KQR tape generator: deterministic quote/trade tapes in the exact
//! format the recorder writes and kairos-replayd / kairos-sim replay (stream 1001
//! market data). Used to exercise the TUI and the trading engine off-hours.
//!
//! Two hard constraints, both proven against the offline fill model:
//!   1. Timestamps land in the continuous TWSE session 09:00-13:25 Taipei. The
//!      engine reads `HhmmFromUs(ts)` in local time; an out-of-window ts routes
//!      every order to the opening auction and never fills. Generation refuses to
//!      leave the window unless `allow_out_of_window` is set (auction/night tests).
//!   2. Fills need Trade prints, not just Quotes. The conservative fill model
//!      (exec/scenario/src/sim/fill_model.cpp `OnTrade`) crosses a resting BUY only
//!      when a trade prints strictly BELOW its price (`price < order.price`); a SELL
//!      only when strictly above; a print AT the price never crosses. Scenarios are
//!      built purely to satisfy or deny that rule.
//!
//! Determinism: no wall clock and no external RNG. The file header's created_ts_us
//! is the session-open epoch, every event ts is a fixed integer offset, and all
//! jitter comes from a seeded hand-rolled LCG. Identical inputs produce a
//! byte-identical tape, which is what the golden regression checksums.

use std::io::{self, Write};

use crate::encode::{encode_quote, encode_trade};
use crate::model::{Exchange, PriceLevel, Quote, QuoteBoard, Session, Trade};
use crate::record::RecordWriter;

pub const STREAM_ID: u32 = 1001;
const OPEN_HHMM: i32 = 900;
const CLOSE_WINDOW_START_HHMM: i32 = 1325;
const DRIFT_PERIOD: i64 = 8;
const PRICE_TICK: i64 = 50;

/// Named scenario, each with a documented expected behavior (see the fixtures
/// README). `QuotesOnly` emits no trades (zero fills possible); `TrendDay` walks a
/// descending trade ladder that crosses a resting join BUY; `LimitLock` pins every
/// trade at the daily limit so a resting BUY at the limit never crosses.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Scenario {
    QuotesOnly,
    TrendDay,
    LimitLock,
}

impl Scenario {
    pub fn parse(s: &str) -> Result<Self, String> {
        match s {
            "quotes-only" => Ok(Scenario::QuotesOnly),
            "trend-day" => Ok(Scenario::TrendDay),
            "limit-lock" => Ok(Scenario::LimitLock),
            other => Err(format!(
                "unknown scenario {other}; expected quotes-only|trend-day|limit-lock"
            )),
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Scenario::QuotesOnly => "quotes-only",
            Scenario::TrendDay => "trend-day",
            Scenario::LimitLock => "limit-lock",
        }
    }
}

/// Fully specifies a tape. Same params in -> byte-identical tape out.
#[derive(Clone, Debug)]
pub struct GenParams {
    pub scenario: Scenario,
    pub symbol: String,
    pub date: u32,
    pub seconds: u32,
    pub tick_ms: u32,
    pub seed: u64,
    pub base_mantissa: i64,
    pub scale: u8,
    pub allow_out_of_window: bool,
}

impl GenParams {
    pub fn session_open_us(&self) -> i64 {
        session_open_us(self.date)
    }

    /// Frozen inputs for the committed `quotes_only_2330.kqr` fixture.
    pub fn fixture_quotes_only() -> Self {
        Self {
            scenario: Scenario::QuotesOnly,
            symbol: "2330".to_owned(),
            date: 20_260_703,
            seconds: 120,
            tick_ms: 1000,
            seed: 1,
            base_mantissa: 58_000,
            scale: 2,
            allow_out_of_window: false,
        }
    }

    /// Frozen inputs for the committed `trend_day_2330.kqr` fixture.
    pub fn fixture_trend_day() -> Self {
        Self {
            scenario: Scenario::TrendDay,
            ..Self::fixture_quotes_only()
        }
    }

    /// Frozen inputs for the committed `limit_lock_2330.kqr` fixture.
    pub fn fixture_limit_lock() -> Self {
        Self {
            scenario: Scenario::LimitLock,
            ..Self::fixture_quotes_only()
        }
    }
}

/// Written-tape summary for the CLI stats line.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Stats {
    pub records: u64,
    pub quotes: u64,
    pub trades: u64,
    pub first_ts_us: i64,
    pub last_ts_us: i64,
}

#[derive(Debug)]
pub enum GenError {
    Io(io::Error),
    /// An event ts fell outside the continuous session and `allow_out_of_window`
    /// was not set; the tape would produce acked-but-0-fill orders.
    OutOfWindow {
        ts_us: i64,
        hhmm: i32,
    },
}

impl From<io::Error> for GenError {
    fn from(e: io::Error) -> Self {
        GenError::Io(e)
    }
}

impl std::fmt::Display for GenError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            GenError::Io(e) => write!(f, "io: {e}"),
            GenError::OutOfWindow { ts_us, hhmm } => write!(
                f,
                "event ts {ts_us} is at {hhmm:04} local, outside the continuous \
                 session 09:00-13:25; pass allow_out_of_window to force it"
            ),
        }
    }
}

impl std::error::Error for GenError {}

/// A single tape event; `recv_ts_us` inside the payload equals the record ts.
enum TapeEvent {
    Quote(Quote),
    Trade(Trade),
}

impl TapeEvent {
    fn ts(&self) -> i64 {
        match self {
            TapeEvent::Quote(q) => q.recv_ts_us,
            TapeEvent::Trade(t) => t.recv_ts_us,
        }
    }
}

/// Small hand-rolled LCG (Knuth MMIX constants) so jitter is seeded and no `rand`
/// dependency is pulled in.
struct Lcg {
    state: u64,
}

impl Lcg {
    fn new(seed: u64) -> Self {
        Self { state: seed }
    }

    fn next_u64(&mut self) -> u64 {
        self.state = self
            .state
            .wrapping_mul(6_364_136_223_846_793_005)
            .wrapping_add(1_442_695_040_888_963_407);
        self.state
    }

    /// Uniform-ish integer in the inclusive range [lo, hi].
    fn in_range(&mut self, lo: i64, hi: i64) -> i64 {
        let span = (hi - lo + 1) as u64;
        lo + ((self.next_u64() >> 33) % span) as i64
    }
}

/// Days since the Unix epoch for a civil date (Howard Hinnant's algorithm).
fn days_from_civil(y: i64, m: u32, d: u32) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400;
    let mp = if m > 2 { m - 3 } else { m + 9 } as i64;
    let doy = (153 * mp + 2) / 5 + d as i64 - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}

/// Epoch microseconds of 09:00 Taipei on `yyyymmdd` (== 01:00 UTC).
pub fn session_open_us(date: u32) -> i64 {
    let y = (date / 10_000) as i64;
    let m = (date / 100) % 100;
    let d = date % 100;
    let days = days_from_civil(y, m, d);
    (days * 86_400 + 3_600) * 1_000_000
}

/// Taipei local hhmm (hour*100 + minute) of an epoch-us ts; mirrors the C++
/// `HhmmFromUs` in exec/scenario/src/sim/session_schedule.h.
pub fn hhmm_from_us(ts_us: i64) -> i32 {
    let mut secs = ts_us / 1_000_000;
    if ts_us < 0 && ts_us % 1_000_000 != 0 {
        secs -= 1;
    }
    let local = secs + 8 * 3_600;
    let tod = ((local % 86_400) + 86_400) % 86_400;
    let hh = (tod / 3_600) as i32;
    let mm = ((tod % 3_600) / 60) as i32;
    hh * 100 + mm
}

/// True when `ts` lands in the continuous session [09:00, 13:25) Taipei.
pub fn in_session_window(ts_us: i64) -> bool {
    (OPEN_HHMM..CLOSE_WINDOW_START_HHMM).contains(&hhmm_from_us(ts_us))
}

fn base_quote(params: &GenParams, ts: i64, seq: u64) -> Quote {
    Quote {
        symbol: params.symbol.clone(),
        exchange: Exchange::Twse,
        quote_ts_us: ts,
        bids: Vec::new(),
        asks: Vec::new(),
        last_price: params.base_mantissa,
        last_scale: params.scale,
        last_volume: 0,
        is_trial: false,
        source: 0,
        seq,
        epoch: 0,
        recv_ts_us: ts,
        board: QuoteBoard::RoundLot,
        session: Session::Day,
        trading_date: params.date,
        simtrade: false,
        underlying_price: 0,
    }
}

fn base_trade(params: &GenParams, ts: i64, seq: u64, price: i64, volume: i64) -> Trade {
    Trade {
        symbol: params.symbol.clone(),
        exchange: Exchange::Twse,
        source: 0,
        seq,
        epoch: 0,
        trade_ts_us: ts,
        recv_ts_us: ts,
        price_mantissa: price,
        price_scale: params.scale,
        volume,
        is_trial: false,
        session: Session::Day,
        trading_date: params.date,
        simtrade: false,
        underlying_price: 0,
    }
}

fn level2(top: i64, step: i64, scale: u8, top_vol: i64) -> Vec<PriceLevel> {
    vec![
        PriceLevel {
            price_mantissa: top,
            price_scale: scale,
            volume: top_vol,
        },
        PriceLevel {
            price_mantissa: top + step,
            price_scale: scale,
            volume: top_vol + 10,
        },
    ]
}

/// Build the full deterministic event sequence for a scenario. Pure: no IO, no
/// wall clock. Shared by the CLI and the golden test so they cannot drift.
fn build_events(params: &GenParams) -> Vec<TapeEvent> {
    let base = params.session_open_us();
    let tick_us = params.tick_ms as i64 * 1_000;
    let span_us = params.seconds as i64 * 1_000_000;
    let mut rng = Lcg::new(params.seed);
    let mut events = Vec::new();
    let mut seq = 0u64;
    let mut i = 0i64;

    while i * tick_us < span_us {
        let ts_q = base + i * tick_us;
        let ts_t = ts_q + tick_us / 2;
        match params.scenario {
            Scenario::QuotesOnly => {
                let mid = params.base_mantissa;
                let bvol = rng.in_range(20, 50);
                let avol = rng.in_range(20, 50);
                let mut q = base_quote(params, ts_q, seq);
                q.bids = level2(mid - PRICE_TICK, -PRICE_TICK, params.scale, bvol);
                q.asks = level2(mid + PRICE_TICK, PRICE_TICK, params.scale, avol);
                q.last_price = mid;
                events.push(TapeEvent::Quote(q));
                seq += 1;
            }
            Scenario::TrendDay => {
                let mid = params.base_mantissa - (i / DRIFT_PERIOD) * PRICE_TICK;
                let best_bid = mid - PRICE_TICK;
                let best_ask = mid + PRICE_TICK;
                let bvol = rng.in_range(20, 50);
                let avol = rng.in_range(20, 50);
                let mut q = base_quote(params, ts_q, seq);
                q.bids = level2(best_bid, -PRICE_TICK, params.scale, bvol);
                q.asks = level2(best_ask, PRICE_TICK, params.scale, avol);
                q.last_price = mid;
                q.last_volume = 1;
                events.push(TapeEvent::Quote(q));
                seq += 1;
                let tvol = rng.in_range(1, 9);
                events.push(TapeEvent::Trade(base_trade(
                    params, ts_t, seq, best_bid, tvol,
                )));
                seq += 1;
            }
            Scenario::LimitLock => {
                let limit = params.base_mantissa;
                let queue = 100 + i * 10;
                let mut q = base_quote(params, ts_q, seq);
                q.bids = vec![PriceLevel {
                    price_mantissa: limit,
                    price_scale: params.scale,
                    volume: queue,
                }];
                q.asks = vec![PriceLevel {
                    price_mantissa: limit,
                    price_scale: params.scale,
                    volume: rng.in_range(1, 5),
                }];
                q.last_price = limit;
                q.last_volume = 1;
                events.push(TapeEvent::Quote(q));
                seq += 1;
                let tvol = rng.in_range(1, 5);
                events.push(TapeEvent::Trade(base_trade(params, ts_t, seq, limit, tvol)));
                seq += 1;
            }
        }
        i += 1;
    }
    events
}

/// Encode and append every scenario event to an already-created KQR writer,
/// returning a summary. Refuses out-of-window timestamps unless the params allow it.
pub fn write_tape<W: Write>(
    w: &mut RecordWriter<W>,
    params: &GenParams,
) -> Result<Stats, GenError> {
    let events = build_events(params);
    if !params.allow_out_of_window {
        for e in &events {
            let ts = e.ts();
            if !in_session_window(ts) {
                return Err(GenError::OutOfWindow {
                    ts_us: ts,
                    hhmm: hhmm_from_us(ts),
                });
            }
        }
    }
    let mut stats = Stats::default();
    for e in &events {
        let payload = match e {
            TapeEvent::Quote(q) => {
                stats.quotes += 1;
                encode_quote(q)
            }
            TapeEvent::Trade(t) => {
                stats.trades += 1;
                encode_trade(t)
            }
        };
        w.append(e.ts(), &payload)?;
        if stats.records == 0 {
            stats.first_ts_us = e.ts();
        }
        stats.last_ts_us = e.ts();
        stats.records += 1;
    }
    Ok(stats)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::decode::{FeedEvent, decode_feed_event};
    use crate::record::{FileHeader, RecordReader};

    fn generate(params: &GenParams) -> Vec<u8> {
        let mut w = RecordWriter::create(
            Vec::new(),
            &FileHeader::new(STREAM_ID, params.session_open_us()),
        )
        .unwrap();
        write_tape(&mut w, params).unwrap();
        w.into_inner()
    }

    fn decode_all(bytes: &[u8]) -> Vec<FeedEvent> {
        let (_, reader) = RecordReader::open(bytes).unwrap();
        reader
            .map(|r| decode_feed_event(&r.unwrap().payload).unwrap())
            .collect()
    }

    #[test]
    fn session_open_lands_at_0900() {
        assert_eq!(hhmm_from_us(session_open_us(20_260_703)), 900);
    }

    #[test]
    fn all_scenarios_stay_in_window() {
        for params in [
            GenParams::fixture_quotes_only(),
            GenParams::fixture_trend_day(),
            GenParams::fixture_limit_lock(),
        ] {
            for e in build_events(&params) {
                assert!(
                    in_session_window(e.ts()),
                    "{} produced ts {} at {}",
                    params.scenario.name(),
                    e.ts(),
                    hhmm_from_us(e.ts())
                );
            }
        }
    }

    #[test]
    fn quotes_only_has_no_trades() {
        let events = decode_all(&generate(&GenParams::fixture_quotes_only()));
        assert!(events.iter().all(|e| matches!(e, FeedEvent::Quote(_))));
        assert!(!events.is_empty());
    }

    #[test]
    fn trend_day_prints_below_bid_reference() {
        let params = GenParams::fixture_trend_day();
        let reference = params.base_mantissa - PRICE_TICK; // initial best bid
        let events = decode_all(&generate(&params));
        let below = events.iter().any(|e| match e {
            FeedEvent::Trade(t) => t.price_mantissa < reference,
            _ => false,
        });
        let trades = events
            .iter()
            .filter(|e| matches!(e, FeedEvent::Trade(_)))
            .count();
        assert!(trades > 0, "trend-day must emit trades to produce fills");
        assert!(
            below,
            "trend-day must print strictly below the join reference"
        );
    }

    #[test]
    fn limit_lock_never_prints_through() {
        let params = GenParams::fixture_limit_lock();
        let limit = params.base_mantissa;
        let events = decode_all(&generate(&params));
        let trades: Vec<_> = events
            .iter()
            .filter_map(|e| match e {
                FeedEvent::Trade(t) => Some(t.price_mantissa),
                _ => None,
            })
            .collect();
        assert!(!trades.is_empty());
        assert!(
            trades.iter().all(|&p| p == limit),
            "limit-lock trades must all print AT the limit (never through)"
        );
    }

    #[test]
    fn generation_is_byte_identical() {
        let params = GenParams::fixture_trend_day();
        assert_eq!(generate(&params), generate(&params));
    }

    #[test]
    fn out_of_window_is_refused() {
        let mut params = GenParams::fixture_quotes_only();
        params.seconds = 6 * 3_600; // pushes past 13:25
        let mut w = RecordWriter::create(
            Vec::new(),
            &FileHeader::new(STREAM_ID, params.session_open_us()),
        )
        .unwrap();
        assert!(matches!(
            write_tape(&mut w, &params),
            Err(GenError::OutOfWindow { .. })
        ));
    }
}
