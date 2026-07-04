use std::collections::BTreeMap;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use kairos_core::decode::{FeedEvent, decode_feed_event};
use kairos_core::encode::encode_subscribe;
use kairos_core::model::PriceLevel;
use kairos_core::uds::frame::{read_frame, write_frame};
use tokio::net::UnixStream;

const INITIAL_BACKOFF: Duration = Duration::from_millis(500);
const MAX_BACKOFF: Duration = Duration::from_secs(5);

#[derive(Clone, Debug)]
pub struct SourceStat {
    pub last_symbol: String,
    pub last_seen: Instant,
    pub count: u64,
}

#[derive(Clone, Debug, Default)]
pub struct SymbolBook {
    pub bids: Vec<PriceLevel>,
    pub asks: Vec<PriceLevel>,
    pub last_price: i64,
    pub last_scale: u8,
    pub last_volume: i64,
    pub is_trial: bool,
    pub source: u16,
    pub last_seen: Option<Instant>,
}

#[derive(Clone, Debug, Default)]
pub struct FeedState {
    pub connected: bool,
    pub last_error: Option<String>,
    pub per_source: BTreeMap<u16, SourceStat>,
    pub per_symbol: BTreeMap<String, SymbolBook>,
}

impl FeedState {
    fn record(&mut self, source: u16, symbol: &str, now: Instant) {
        self.per_source
            .entry(source)
            .and_modify(|s| {
                s.last_symbol = symbol.to_string();
                s.last_seen = now;
                s.count += 1;
            })
            .or_insert(SourceStat {
                last_symbol: symbol.to_string(),
                last_seen: now,
                count: 1,
            });
    }

    pub fn apply(&mut self, event: &FeedEvent, now: Instant) {
        match event {
            FeedEvent::Quote(q) => {
                self.record(q.source, &q.symbol, now);
                let book = self.per_symbol.entry(q.symbol.clone()).or_default();
                book.bids = q.bids.clone();
                book.asks = q.asks.clone();
                if q.last_price != 0 {
                    book.last_price = q.last_price;
                    book.last_scale = q.last_scale;
                    book.last_volume = q.last_volume;
                }
                book.is_trial = q.is_trial;
                book.source = q.source;
                book.last_seen = Some(now);
            }
            FeedEvent::Trade(t) => {
                self.record(t.source, &t.symbol, now);
                let book = self.per_symbol.entry(t.symbol.clone()).or_default();
                book.last_price = t.price_mantissa;
                book.last_scale = t.price_scale;
                book.last_volume = t.volume;
                book.is_trial = t.is_trial;
                book.source = t.source;
                book.last_seen = Some(now);
            }
        }
    }
}

/// Connect to the UDS quote socket, subscribe to `symbols`, and fold every
/// decoded feed event into `state`. Reconnects forever with capped backoff; the
/// only write is the Subscribe frame. Never panics.
pub async fn run(socket_path: String, symbols: Vec<String>, state: Arc<Mutex<FeedState>>) {
    let mut backoff = INITIAL_BACKOFF;
    loop {
        match UnixStream::connect(&socket_path).await {
            Ok(stream) => {
                backoff = INITIAL_BACKOFF;
                {
                    let mut s = state.lock().unwrap();
                    s.connected = true;
                    s.last_error = None;
                }
                if let Err(e) = stream_quotes(stream, &symbols, &state).await {
                    state.lock().unwrap().last_error = Some(e.to_string());
                }
                state.lock().unwrap().connected = false;
            }
            Err(e) => {
                let mut s = state.lock().unwrap();
                s.connected = false;
                s.last_error = Some(e.to_string());
            }
        }
        tokio::time::sleep(backoff).await;
        backoff = (backoff * 2).min(MAX_BACKOFF);
    }
}

async fn stream_quotes(
    stream: UnixStream,
    symbols: &[String],
    state: &Arc<Mutex<FeedState>>,
) -> std::io::Result<()> {
    let (mut rd, mut wr) = stream.into_split();
    let refs: Vec<&str> = symbols.iter().map(String::as_str).collect();
    write_frame(&mut wr, &encode_subscribe(&refs)).await?;
    while let Some(frame) = read_frame(&mut rd).await? {
        if let Ok(event) = decode_feed_event(&frame) {
            state.lock().unwrap().apply(&event, Instant::now());
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use kairos_core::encode::{encode_quote, encode_trade};
    use kairos_core::model::{Exchange, Quote, QuoteBoard, Session, Trade};

    fn level(mantissa: i64, volume: i64) -> PriceLevel {
        PriceLevel {
            price_mantissa: mantissa,
            price_scale: 2,
            volume,
        }
    }

    fn quote(symbol: &str, source: u16) -> Vec<u8> {
        quote_with_levels(symbol, source, vec![], vec![], 100, 1)
    }

    fn quote_with_levels(
        symbol: &str,
        source: u16,
        bids: Vec<PriceLevel>,
        asks: Vec<PriceLevel>,
        last_price: i64,
        last_volume: i64,
    ) -> Vec<u8> {
        encode_quote(&Quote {
            symbol: symbol.to_string(),
            exchange: Exchange::Twse,
            quote_ts_us: 0,
            bids,
            asks,
            last_price,
            last_scale: 2,
            last_volume,
            is_trial: false,
            source,
            seq: 0,
            epoch: 0,
            recv_ts_us: 0,
            board: QuoteBoard::RoundLot,
            session: Session::Day,
            trading_date: 0,
            simtrade: false,
            underlying_price: 0,
        })
    }

    fn trade(symbol: &str, source: u16, price_mantissa: i64, volume: i64) -> Vec<u8> {
        encode_trade(&Trade {
            symbol: symbol.to_string(),
            exchange: Exchange::Twse,
            source,
            seq: 0,
            epoch: 0,
            trade_ts_us: 0,
            recv_ts_us: 0,
            price_mantissa,
            price_scale: 2,
            volume,
            is_trial: true,
            session: Session::Day,
            trading_date: 0,
            simtrade: false,
            underlying_price: 0,
        })
    }

    #[test]
    fn quote_populates_and_replaces_book() {
        let mut st = FeedState::default();
        let now = Instant::now();
        st.apply(
            &decode_feed_event(&quote_with_levels(
                "2330",
                0,
                vec![level(58000, 5)],
                vec![level(58100, 3)],
                58050,
                2,
            ))
            .unwrap(),
            now,
        );
        let b = &st.per_symbol["2330"];
        assert_eq!(b.bids.len(), 1);
        assert_eq!(b.asks.len(), 1);
        assert_eq!(b.last_price, 58050);

        st.apply(
            &decode_feed_event(&quote_with_levels(
                "2330",
                0,
                vec![level(57000, 1), level(56900, 2)],
                vec![],
                57050,
                4,
            ))
            .unwrap(),
            now,
        );
        let b = &st.per_symbol["2330"];
        assert_eq!(b.bids.len(), 2, "levels replaced, not appended");
        assert!(b.asks.is_empty());
        assert_eq!(b.last_price, 57050);
    }

    #[test]
    fn trade_updates_last_without_clobbering_levels() {
        let mut st = FeedState::default();
        let now = Instant::now();
        st.apply(
            &decode_feed_event(&quote_with_levels(
                "2330",
                0,
                vec![level(58000, 5)],
                vec![level(58100, 3)],
                58050,
                2,
            ))
            .unwrap(),
            now,
        );
        st.apply(
            &decode_feed_event(&trade("2330", 0, 58080, 9)).unwrap(),
            now,
        );
        let b = &st.per_symbol["2330"];
        assert_eq!(b.bids.len(), 1, "trade must not clear book levels");
        assert_eq!(b.asks.len(), 1);
        assert_eq!(b.last_price, 58080);
        assert_eq!(b.last_volume, 9);
        assert!(b.is_trial);
    }

    #[test]
    fn depth_only_quote_keeps_last_trade() {
        let mut st = FeedState::default();
        let now = Instant::now();
        st.apply(
            &decode_feed_event(&trade("2330", 0, 58080, 9)).unwrap(),
            now,
        );
        st.apply(
            &decode_feed_event(&quote_with_levels(
                "2330",
                0,
                vec![level(58000, 5)],
                vec![level(58100, 3)],
                0,
                0,
            ))
            .unwrap(),
            now,
        );
        let b = &st.per_symbol["2330"];
        assert_eq!(b.bids.len(), 1, "depth-only quote still refreshes levels");
        assert_eq!(
            b.last_price, 58080,
            "last trade must survive a depth-only quote"
        );
        assert_eq!(b.last_volume, 9);
    }

    #[test]
    fn trade_for_unseen_symbol_creates_entry() {
        let mut st = FeedState::default();
        let now = Instant::now();
        st.apply(&decode_feed_event(&trade("6666", 3, 1200, 7)).unwrap(), now);
        let b = &st.per_symbol["6666"];
        assert!(b.bids.is_empty());
        assert_eq!(b.last_price, 1200);
        assert_eq!(b.source, 3);
    }

    #[test]
    fn apply_tracks_per_source() {
        let mut st = FeedState::default();
        let now = Instant::now();
        st.apply(&decode_feed_event(&quote("2330", 0)).unwrap(), now);
        st.apply(&decode_feed_event(&quote("0050", 0)).unwrap(), now);
        st.apply(&decode_feed_event(&quote("2317", 7)).unwrap(), now);

        assert_eq!(st.per_source.len(), 2);
        let s0 = &st.per_source[&0];
        assert_eq!(s0.count, 2);
        assert_eq!(s0.last_symbol, "0050");
        assert_eq!(st.per_source[&7].count, 1);
        assert_eq!(st.per_source[&7].last_symbol, "2317");
    }
}
