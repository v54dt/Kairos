#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Exchange {
    Twse,
    Tpex,
    Tfx,
    Otc,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum QuoteBoard {
    #[default]
    Unknown,
    RoundLot,
    OddLot,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Session {
    #[default]
    Unknown,
    Day,
    Night,
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
    pub source: u16,
    pub seq: u64,
    pub epoch: u32,
    pub recv_ts_us: i64,
    pub board: QuoteBoard,
    pub session: Session,
    pub trading_date: u32,
    pub simtrade: bool,
    pub underlying_price: i64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Trade {
    pub symbol: String,
    pub exchange: Exchange,
    pub source: u16,
    pub seq: u64,
    pub epoch: u32,
    pub trade_ts_us: i64,
    pub recv_ts_us: i64,
    pub price_mantissa: i64,
    pub price_scale: u8,
    pub volume: i64,
    pub is_trial: bool,
    pub session: Session,
    pub trading_date: u32,
    pub simtrade: bool,
    pub underlying_price: i64,
}
