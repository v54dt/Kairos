use crate::kairos_capnp;
use crate::model::{Exchange, PriceLevel, Quote};

#[derive(Debug)]
pub enum DecodeError {
    Capnp(capnp::Error),
    NotInSchema(capnp::NotInSchema),
    Utf8(core::str::Utf8Error),
    NotAQuote,
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
}
