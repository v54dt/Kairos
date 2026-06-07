use std::collections::HashMap;

use crate::model::Quote;

#[derive(Default)]
pub struct Book {
    quotes: HashMap<String, Quote>,
}

impl Book {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn update(&mut self, quote: Quote) {
        self.quotes.insert(quote.symbol.clone(), quote);
    }

    pub fn get(&self, symbol: &str) -> Option<&Quote> {
        self.quotes.get(symbol)
    }

    pub fn len(&self) -> usize {
        self.quotes.len()
    }

    pub fn is_empty(&self) -> bool {
        self.quotes.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{Exchange, PriceLevel};

    fn quote(symbol: &str, last: i64) -> Quote {
        Quote {
            symbol: symbol.to_owned(),
            exchange: Exchange::Twse,
            quote_ts_us: 0,
            bids: vec![PriceLevel {
                price_mantissa: last - 50,
                price_scale: 2,
                volume: 1,
            }],
            asks: vec![PriceLevel {
                price_mantissa: last + 50,
                price_scale: 2,
                volume: 1,
            }],
            last_price: last,
            last_scale: 2,
            last_volume: 1,
            is_trial: false,
        }
    }

    #[test]
    fn update_then_get_latest() {
        let mut book = Book::new();
        assert!(book.is_empty());
        book.update(quote("2330", 58000));
        book.update(quote("2317", 11000));
        book.update(quote("2330", 58100));

        assert_eq!(book.len(), 2);
        assert_eq!(book.get("2330").unwrap().last_price, 58100);
        assert_eq!(book.get("2317").unwrap().last_price, 11000);
        assert!(book.get("9999").is_none());
    }
}
