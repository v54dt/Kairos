#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Exchange {
    Twse,
    Tpex,
    Tfx,
    Otc,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PriceLevel {
    pub price_mantissa: i64,
    pub price_scale: u8,
    pub volume: i64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Quote {
    pub symbol: String,
    pub exchange: Exchange,
    pub quote_ts_us: i64,
    pub bids: Vec<PriceLevel>,
    pub asks: Vec<PriceLevel>,
    pub last_price: i64,
    pub last_scale: u8,
    pub last_volume: i64,
    pub is_trial: bool,
}

pub fn book_key(quote: &Quote) -> &str {
    &quote.symbol
}
