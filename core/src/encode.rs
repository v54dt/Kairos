use capnp::message::Builder;
use capnp::serialize;

use crate::kairos_capnp;
use crate::model::{Exchange, PriceLevel, Quote};

fn cap_exchange(e: Exchange) -> kairos_capnp::Exchange {
    match e {
        Exchange::Twse => kairos_capnp::Exchange::Twse,
        Exchange::Tpex => kairos_capnp::Exchange::Tpex,
        Exchange::Tfx => kairos_capnp::Exchange::Tfx,
        Exchange::Otc => kairos_capnp::Exchange::Otc,
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
        fill_levels(quote.reborrow().init_bids(q.bids.len() as u32), &q.bids);
        fill_levels(quote.init_asks(q.asks.len() as u32), &q.asks);
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
        };

        let bytes = encode_quote(&q);
        let reader = serialize::read_message(&mut &bytes[..], ReaderOptions::new()).unwrap();
        let env = reader.get_root::<kairos_capnp::envelope::Reader>().unwrap();
        let decoded = decode_envelope(env).unwrap();

        assert_eq!(decoded, q);
    }
}
