use capnp::message::Builder;
use capnp::serialize;

use crate::kairos_capnp;
use crate::model::{Exchange, PriceLevel, Quote, QuoteBoard, Session, Trade};

fn cap_exchange(e: Exchange) -> kairos_capnp::Exchange {
    match e {
        Exchange::Twse => kairos_capnp::Exchange::Twse,
        Exchange::Tpex => kairos_capnp::Exchange::Tpex,
        Exchange::Tfx => kairos_capnp::Exchange::Tfx,
        Exchange::Otc => kairos_capnp::Exchange::Otc,
    }
}

fn cap_board(b: QuoteBoard) -> kairos_capnp::QuoteBoard {
    match b {
        QuoteBoard::Unknown => kairos_capnp::QuoteBoard::Unknown,
        QuoteBoard::RoundLot => kairos_capnp::QuoteBoard::RoundLot,
        QuoteBoard::OddLot => kairos_capnp::QuoteBoard::OddLot,
    }
}

fn cap_session(s: Session) -> kairos_capnp::Session {
    match s {
        Session::Unknown => kairos_capnp::Session::Unknown,
        Session::Day => kairos_capnp::Session::Day,
        Session::Night => kairos_capnp::Session::Night,
    }
}

fn fill_levels(
    mut out: capnp::struct_list::Builder<'_, kairos_capnp::price_level::Owned>,
    levels: &[PriceLevel],
) {
    for (i, lvl) in levels.iter().enumerate() {
        let mut b = out.reborrow().get(i as u32);
        b.set_price_mantissa(lvl.price_mantissa);
        b.set_price_scale(lvl.price_scale);
        b.set_volume(lvl.volume);
    }
}

pub fn encode_quote(q: &Quote) -> Vec<u8> {
    let mut msg = Builder::new_default();
    {
        let env = msg.init_root::<kairos_capnp::envelope::Builder>();
        let mut quote = env.init_quote();
        quote.set_symbol(q.symbol.as_str());
        quote.set_exchange(cap_exchange(q.exchange));
        quote.set_quote_ts_us(q.quote_ts_us);
        quote.set_last_price(q.last_price);
        quote.set_last_scale(q.last_scale);
        quote.set_last_volume(q.last_volume);
        quote.set_is_trial(q.is_trial);
        quote.set_source(q.source);
        quote.set_seq(q.seq);
        quote.set_epoch(q.epoch);
        quote.set_recv_ts_us(q.recv_ts_us);
        quote.set_board(cap_board(q.board));
        quote.set_session(cap_session(q.session));
        quote.set_trading_date(q.trading_date);
        quote.set_simtrade(q.simtrade);
        quote.set_underlying_price(q.underlying_price);
        fill_levels(quote.reborrow().init_bids(q.bids.len() as u32), &q.bids);
        fill_levels(quote.init_asks(q.asks.len() as u32), &q.asks);
    }
    let mut buf = Vec::new();
    serialize::write_message(&mut buf, &msg).unwrap();
    buf
}

pub fn encode_trade(t: &Trade) -> Vec<u8> {
    let mut msg = Builder::new_default();
    {
        let env = msg.init_root::<kairos_capnp::envelope::Builder>();
        let mut trade = env.init_trade();
        trade.set_symbol(t.symbol.as_str());
        trade.set_exchange(cap_exchange(t.exchange));
        trade.set_source(t.source);
        trade.set_seq(t.seq);
        trade.set_epoch(t.epoch);
        trade.set_trade_ts_us(t.trade_ts_us);
        trade.set_recv_ts_us(t.recv_ts_us);
        trade.set_price_mantissa(t.price_mantissa);
        trade.set_price_scale(t.price_scale);
        trade.set_volume(t.volume);
        trade.set_is_trial(t.is_trial);
        trade.set_session(cap_session(t.session));
        trade.set_trading_date(t.trading_date);
        trade.set_simtrade(t.simtrade);
        trade.set_underlying_price(t.underlying_price);
    }
    let mut buf = Vec::new();
    serialize::write_message(&mut buf, &msg).unwrap();
    buf
}

pub fn encode_subscribe(symbols: &[&str]) -> Vec<u8> {
    let mut msg = Builder::new_default();
    {
        let env = msg.init_root::<kairos_capnp::envelope::Builder>();
        let sub = env.init_subscribe();
        let mut list = sub.init_symbols(symbols.len() as u32);
        for (i, s) in symbols.iter().enumerate() {
            list.set(i as u32, *s);
        }
    }
    let mut buf = Vec::new();
    serialize::write_message(&mut buf, &msg).unwrap();
    buf
}

pub fn encode_sub_ack(symbols: &[String], ok: bool) -> Vec<u8> {
    let mut msg = Builder::new_default();
    {
        let env = msg.init_root::<kairos_capnp::envelope::Builder>();
        let mut ack = env.init_sub_ack();
        ack.set_ok(ok);
        let mut list = ack.init_symbols(symbols.len() as u32);
        for (i, s) in symbols.iter().enumerate() {
            list.set(i as u32, s.as_str());
        }
    }
    let mut buf = Vec::new();
    serialize::write_message(&mut buf, &msg).unwrap();
    buf
}

pub fn encode_error(text: &str) -> Vec<u8> {
    let mut msg = Builder::new_default();
    {
        let mut env = msg.init_root::<kairos_capnp::envelope::Builder>();
        env.set_error(text);
    }
    let mut buf = Vec::new();
    serialize::write_message(&mut buf, &msg).unwrap();
    buf
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::decode::decode_envelope;
    use capnp::message::ReaderOptions;

    #[test]
    fn encode_then_decode_roundtrip() {
        let q = Quote {
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
                    price_mantissa: 57950,
                    price_scale: 2,
                    volume: 50,
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
            seq: 17,
            epoch: 2,
            recv_ts_us: 1_700_000_000_000_009,
            board: crate::model::QuoteBoard::RoundLot,
            session: crate::model::Session::Day,
            trading_date: 20_260_704,
            simtrade: false,
            underlying_price: 0,
        };

        let bytes = encode_quote(&q);
        let reader = serialize::read_message(&mut &bytes[..], ReaderOptions::new()).unwrap();
        let env = reader.get_root::<kairos_capnp::envelope::Reader>().unwrap();
        let decoded = decode_envelope(env).unwrap();

        assert_eq!(decoded, q);
    }

    #[test]
    fn trade_encode_then_decode_roundtrip() {
        let t = Trade {
            symbol: "2317".to_owned(),
            exchange: Exchange::Twse,
            source: 0,
            seq: 99,
            epoch: 4,
            trade_ts_us: 1_700_000_000_000_100,
            recv_ts_us: 1_700_000_000_000_101,
            price_mantissa: 11050,
            price_scale: 2,
            volume: 3,
            is_trial: true,
            session: crate::model::Session::Day,
            trading_date: 20_260_704,
            simtrade: false,
            underlying_price: 0,
        };
        let bytes = encode_trade(&t);
        match crate::decode::decode_feed_event(&bytes).unwrap() {
            crate::decode::FeedEvent::Trade(got) => assert_eq!(got, t),
            other => panic!("expected trade, got {other:?}"),
        }
    }

    #[test]
    fn subscribe_roundtrip() {
        let bytes = encode_subscribe(&["2330", "2317"]);
        match crate::decode::decode_message_bytes(&bytes).unwrap() {
            crate::decode::Message::Subscribe(syms) => {
                assert_eq!(syms, vec!["2330".to_string(), "2317".to_string()]);
            }
            other => panic!("expected subscribe, got {other:?}"),
        }
    }

    #[test]
    fn sub_ack_roundtrip() {
        let bytes = encode_sub_ack(&["2330".to_string(), "0050".to_string()], true);
        let reader = serialize::read_message(&mut &bytes[..], ReaderOptions::new()).unwrap();
        let env = reader.get_root::<kairos_capnp::envelope::Reader>().unwrap();
        match env.which().unwrap() {
            kairos_capnp::envelope::Which::SubAck(a) => {
                let a = a.unwrap();
                assert!(a.get_ok());
                let syms = a.get_symbols().unwrap();
                assert_eq!(syms.len(), 2);
                assert_eq!(syms.get(0).unwrap().to_string().unwrap(), "2330");
            }
            _ => panic!("expected subAck"),
        }
    }

    #[test]
    fn error_roundtrip() {
        let bytes = encode_error("lagged 5");
        let reader = serialize::read_message(&mut &bytes[..], ReaderOptions::new()).unwrap();
        let env = reader.get_root::<kairos_capnp::envelope::Reader>().unwrap();
        match env.which().unwrap() {
            kairos_capnp::envelope::Which::Error(e) => {
                assert_eq!(e.unwrap().to_string().unwrap(), "lagged 5");
            }
            _ => panic!("expected error"),
        }
    }
}
