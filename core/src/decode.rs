use crate::kairos_capnp;
use crate::model::{Exchange, PriceLevel, Quote, QuoteBoard, Session, Trade};

#[derive(Debug)]
pub enum DecodeError {
    Capnp(capnp::Error),
    NotInSchema(capnp::NotInSchema),
    Utf8(core::str::Utf8Error),
    NotAQuote,
    Unsupported,
    /// A well-formed Envelope whose variant this router does not handle (e.g. a
    /// control frame on the data stream, or a future variant an old build cannot
    /// route). Distinct from a genuine capnp parse failure.
    UnknownVariant,
}

impl From<capnp::Error> for DecodeError {
    fn from(e: capnp::Error) -> Self {
        DecodeError::Capnp(e)
    }
}

impl From<capnp::NotInSchema> for DecodeError {
    fn from(e: capnp::NotInSchema) -> Self {
        DecodeError::NotInSchema(e)
    }
}

impl From<core::str::Utf8Error> for DecodeError {
    fn from(e: core::str::Utf8Error) -> Self {
        DecodeError::Utf8(e)
    }
}

fn map_exchange(e: kairos_capnp::Exchange) -> Exchange {
    match e {
        kairos_capnp::Exchange::Twse => Exchange::Twse,
        kairos_capnp::Exchange::Tpex => Exchange::Tpex,
        kairos_capnp::Exchange::Tfx => Exchange::Tfx,
        kairos_capnp::Exchange::Otc => Exchange::Otc,
    }
}

fn map_board(b: kairos_capnp::QuoteBoard) -> QuoteBoard {
    match b {
        kairos_capnp::QuoteBoard::Unknown => QuoteBoard::Unknown,
        kairos_capnp::QuoteBoard::RoundLot => QuoteBoard::RoundLot,
        kairos_capnp::QuoteBoard::OddLot => QuoteBoard::OddLot,
    }
}

fn map_session(s: kairos_capnp::Session) -> Session {
    match s {
        kairos_capnp::Session::Unknown => Session::Unknown,
        kairos_capnp::Session::Day => Session::Day,
        kairos_capnp::Session::Night => Session::Night,
    }
}

fn decode_levels(
    levels: capnp::struct_list::Reader<'_, kairos_capnp::price_level::Owned>,
) -> Vec<PriceLevel> {
    levels
        .iter()
        .map(|l| PriceLevel {
            price_mantissa: l.get_price_mantissa(),
            price_scale: l.get_price_scale(),
            volume: l.get_volume(),
        })
        .collect()
}

pub fn decode_quote(q: kairos_capnp::quote::Reader) -> Result<Quote, DecodeError> {
    Ok(Quote {
        symbol: q.get_symbol()?.to_string()?,
        exchange: map_exchange(q.get_exchange()?),
        quote_ts_us: q.get_quote_ts_us(),
        bids: decode_levels(q.get_bids()?),
        asks: decode_levels(q.get_asks()?),
        last_price: q.get_last_price(),
        last_scale: q.get_last_scale(),
        last_volume: q.get_last_volume(),
        is_trial: q.get_is_trial(),
        source: q.get_source(),
        seq: q.get_seq(),
        epoch: q.get_epoch(),
        recv_ts_us: q.get_recv_ts_us(),
        board: map_board(q.get_board()?),
        session: map_session(q.get_session()?),
        trading_date: q.get_trading_date(),
        simtrade: q.get_simtrade(),
        underlying_price: q.get_underlying_price(),
    })
}

pub fn decode_trade(t: kairos_capnp::trade::Reader) -> Result<Trade, DecodeError> {
    Ok(Trade {
        symbol: t.get_symbol()?.to_string()?,
        exchange: map_exchange(t.get_exchange()?),
        source: t.get_source(),
        seq: t.get_seq(),
        epoch: t.get_epoch(),
        trade_ts_us: t.get_trade_ts_us(),
        recv_ts_us: t.get_recv_ts_us(),
        price_mantissa: t.get_price_mantissa(),
        price_scale: t.get_price_scale(),
        volume: t.get_volume(),
        is_trial: t.get_is_trial(),
        session: map_session(t.get_session()?),
        trading_date: t.get_trading_date(),
        simtrade: t.get_simtrade(),
        underlying_price: t.get_underlying_price(),
    })
}

pub fn decode_envelope(env: kairos_capnp::envelope::Reader) -> Result<Quote, DecodeError> {
    match env.which()? {
        kairos_capnp::envelope::Which::Quote(q) => decode_quote(q?),
        _ => Err(DecodeError::NotAQuote),
    }
}

pub fn decode_quote_bytes(bytes: &[u8]) -> Result<Quote, DecodeError> {
    let reader =
        capnp::serialize::read_message(&mut &bytes[..], capnp::message::ReaderOptions::new())?;
    let env = reader.get_root::<kairos_capnp::envelope::Reader>()?;
    decode_envelope(env)
}

/// A decoded market-data event off the feed: either a depth-bearing Quote or a
/// standalone Trade. Both carry a symbol and route to the same subscription.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum FeedEvent {
    Quote(Quote),
    Trade(Trade),
}

impl FeedEvent {
    pub fn symbol(&self) -> &str {
        match self {
            FeedEvent::Quote(q) => &q.symbol,
            FeedEvent::Trade(t) => &t.symbol,
        }
    }
}

/// Decode a data-stream Envelope into a FeedEvent. A well-formed but unrouted
/// variant (control frame / future variant) is `UnknownVariant`; malformed bytes
/// surface as `Capnp`. The caller distinguishes the two for metrics.
pub fn decode_feed_event(bytes: &[u8]) -> Result<FeedEvent, DecodeError> {
    let reader =
        capnp::serialize::read_message(&mut &bytes[..], capnp::message::ReaderOptions::new())?;
    let env = reader.get_root::<kairos_capnp::envelope::Reader>()?;
    match env.which() {
        Ok(kairos_capnp::envelope::Which::Quote(q)) => Ok(FeedEvent::Quote(decode_quote(q?)?)),
        Ok(kairos_capnp::envelope::Which::Trade(t)) => Ok(FeedEvent::Trade(decode_trade(t?)?)),
        // A known-but-unrouted variant (control frame) and a future variant this
        // build's schema does not know (which() -> NotInSchema) are both benign
        // forward-compat traffic, not corruption.
        Ok(_) | Err(_) => Err(DecodeError::UnknownVariant),
    }
}

#[derive(Debug, PartialEq, Eq)]
pub enum Message {
    Quote(Quote),
    Subscribe(Vec<String>),
    Unsubscribe(Vec<String>),
}

fn decode_symbols(list: capnp::text_list::Reader<'_>) -> Result<Vec<String>, DecodeError> {
    let mut out = Vec::with_capacity(list.len() as usize);
    for i in 0..list.len() {
        out.push(list.get(i)?.to_string()?);
    }
    Ok(out)
}

pub fn decode_message_bytes(bytes: &[u8]) -> Result<Message, DecodeError> {
    let reader =
        capnp::serialize::read_message(&mut &bytes[..], capnp::message::ReaderOptions::new())?;
    let env = reader.get_root::<kairos_capnp::envelope::Reader>()?;
    match env.which()? {
        kairos_capnp::envelope::Which::Quote(q) => Ok(Message::Quote(decode_quote(q?)?)),
        kairos_capnp::envelope::Which::Subscribe(s) => {
            Ok(Message::Subscribe(decode_symbols(s?.get_symbols()?)?))
        }
        kairos_capnp::envelope::Which::Unsubscribe(u) => {
            Ok(Message::Unsubscribe(decode_symbols(u?.get_symbols()?)?))
        }
        _ => Err(DecodeError::Unsupported),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use capnp::message::{Builder, ReaderOptions};
    use capnp::serialize;

    fn build_quote_envelope() -> Vec<u8> {
        let mut msg = Builder::new_default();
        {
            let env = msg.init_root::<kairos_capnp::envelope::Builder>();
            let mut q = env.init_quote();
            q.set_symbol("2330");
            q.set_exchange(kairos_capnp::Exchange::Twse);
            q.set_quote_ts_us(1_700_000_000_000_000);
            q.set_last_price(58050);
            q.set_last_scale(2);
            q.set_last_volume(10);
            q.set_is_trial(false);
            {
                let mut bids = q.reborrow().init_bids(2);
                let mut b0 = bids.reborrow().get(0);
                b0.set_price_mantissa(58000);
                b0.set_price_scale(2);
                b0.set_volume(100);
                let mut b1 = bids.reborrow().get(1);
                b1.set_price_mantissa(57950);
                b1.set_price_scale(2);
                b1.set_volume(50);
            }
            let mut asks = q.init_asks(1);
            let mut a0 = asks.reborrow().get(0);
            a0.set_price_mantissa(58100);
            a0.set_price_scale(2);
            a0.set_volume(80);
        }
        let mut buf = Vec::new();
        serialize::write_message(&mut buf, &msg).unwrap();
        buf
    }

    #[test]
    fn decode_envelope_roundtrip() {
        let bytes = build_quote_envelope();
        let reader = serialize::read_message(&mut &bytes[..], ReaderOptions::new()).unwrap();
        let env = reader.get_root::<kairos_capnp::envelope::Reader>().unwrap();
        let q = decode_envelope(env).unwrap();

        assert_eq!(q.symbol, "2330");
        assert_eq!(q.exchange, Exchange::Twse);
        assert_eq!(q.quote_ts_us, 1_700_000_000_000_000);
        assert_eq!(q.bids.len(), 2);
        assert_eq!(q.bids[0].price_mantissa, 58000);
        assert_eq!(q.bids[0].volume, 100);
        assert_eq!(q.bids[1].price_mantissa, 57950);
        assert_eq!(q.asks.len(), 1);
        assert_eq!(q.asks[0].price_mantissa, 58100);
        assert_eq!(q.last_price, 58050);
        assert_eq!(q.last_scale, 2);
        assert!(!q.is_trial);
    }

    #[test]
    fn legacy_quote_defaults_new_fields() {
        // A Quote encoded without any of the A2 fields must decode to the
        // append-only defaults (source 0, seq/epoch 0, board/session Unknown).
        let bytes = build_quote_envelope();
        let q = decode_quote_bytes(&bytes).unwrap();
        assert_eq!(q.source, 0);
        assert_eq!(q.seq, 0);
        assert_eq!(q.epoch, 0);
        assert_eq!(q.recv_ts_us, 0);
        assert_eq!(q.board, QuoteBoard::Unknown);
        assert_eq!(q.session, Session::Unknown);
        assert_eq!(q.trading_date, 0);
        assert!(!q.simtrade);
        assert_eq!(q.underlying_price, 0);
    }

    #[test]
    fn feed_event_trade_roundtrips() {
        let mut msg = Builder::new_default();
        {
            let env = msg.init_root::<kairos_capnp::envelope::Builder>();
            let mut t = env.init_trade();
            t.set_symbol("2330");
            t.set_exchange(kairos_capnp::Exchange::Twse);
            t.set_source(0);
            t.set_seq(42);
            t.set_epoch(3);
            t.set_trade_ts_us(1_700_000_000_000_001);
            t.set_recv_ts_us(1_700_000_000_000_002);
            t.set_price_mantissa(58050);
            t.set_price_scale(2);
            t.set_volume(7);
            t.set_is_trial(true);
        }
        let mut buf = Vec::new();
        serialize::write_message(&mut buf, &msg).unwrap();

        match decode_feed_event(&buf).unwrap() {
            FeedEvent::Trade(t) => {
                assert_eq!(t.symbol, "2330");
                assert_eq!(t.seq, 42);
                assert_eq!(t.epoch, 3);
                assert_eq!(t.price_mantissa, 58050);
                assert_eq!(t.volume, 7);
                assert!(t.is_trial);
                assert_eq!(t.session, Session::Unknown);
            }
            other => panic!("expected trade, got {other:?}"),
        }
    }

    #[test]
    fn feed_event_unknown_variant() {
        // A heartbeat on the data stream is well-formed but not a feed event.
        let mut msg = Builder::new_default();
        msg.init_root::<kairos_capnp::envelope::Builder>()
            .set_heartbeat(());
        let mut buf = Vec::new();
        serialize::write_message(&mut buf, &msg).unwrap();
        assert!(matches!(
            decode_feed_event(&buf),
            Err(DecodeError::UnknownVariant)
        ));
    }

    #[test]
    fn feed_event_malformed_is_capnp_err() {
        assert!(matches!(
            decode_feed_event(&[0xde, 0xad, 0xbe, 0xef]),
            Err(DecodeError::Capnp(_))
        ));
    }
}
