use std::collections::BTreeMap;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use kairos_core::decode::{FeedEvent, decode_feed_event};
use kairos_core::encode::encode_subscribe;
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
pub struct FeedState {
    pub connected: bool,
    pub last_error: Option<String>,
    pub per_source: BTreeMap<u16, SourceStat>,
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
            FeedEvent::Quote(q) => self.record(q.source, &q.symbol, now),
            FeedEvent::Trade(t) => self.record(t.source, &t.symbol, now),
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
    use kairos_core::encode::encode_quote;
    use kairos_core::model::{Exchange, Quote, QuoteBoard, Session};

    fn quote(symbol: &str, source: u16) -> Vec<u8> {
        encode_quote(&Quote {
            symbol: symbol.to_string(),
            exchange: Exchange::Twse,
            quote_ts_us: 0,
            bids: vec![],
            asks: vec![],
            last_price: 100,
            last_scale: 2,
            last_volume: 1,
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
