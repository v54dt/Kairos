use std::collections::HashMap;

use crate::model::Quote;

/// Result of offering a quote to the book: whether it became the latest for its
/// `(source, symbol)` or was rejected as out-of-order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Admit {
    Admitted,
    Dropped,
}

/// Latest-book keyed by `(source, symbol)` so two feeds for the same symbol keep
/// independent books and can never clobber each other. Within one key an update
/// is admitted only if its `(epoch, seq)` is not older than the held one; epoch
/// takes precedence over seq (a higher epoch is always newer, even if seq reset
/// on a session rebuild — see schema/NORMALIZATION.md §3).
#[derive(Default)]
pub struct Book {
    quotes: HashMap<(u16, String), Quote>,
}

impl Book {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn update(&mut self, quote: Quote) -> Admit {
        let key = (quote.source, quote.symbol.clone());
        if let Some(existing) = self.quotes.get(&key)
            && (quote.epoch, quote.seq) < (existing.epoch, existing.seq)
        {
            return Admit::Dropped;
        }
        self.quotes.insert(key, quote);
        Admit::Admitted
    }

    pub fn get(&self, source: u16, symbol: &str) -> Option<&Quote> {
        self.quotes.get(&(source, symbol.to_owned()))
    }

    /// Resolve a symbol against a preference-ordered list of sources, returning the
    /// first source that holds it. Used by the snapshot path to serve the active
    /// (failover-selected) source, falling back to a source that has the symbol.
    pub fn get_preferred(&self, sources: &[u16], symbol: &str) -> Option<&Quote> {
        sources.iter().find_map(|&source| self.get(source, symbol))
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

    fn quote_at(symbol: &str, source: u16, epoch: u32, seq: u64, last: i64) -> Quote {
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
            source,
            seq,
            epoch,
            recv_ts_us: 0,
            board: crate::model::QuoteBoard::RoundLot,
            session: crate::model::Session::Unknown,
            trading_date: 0,
            simtrade: false,
            underlying_price: 0,
        }
    }

    fn quote(symbol: &str, last: i64) -> Quote {
        quote_at(symbol, 0, 0, 0, last)
    }

    #[test]
    fn update_then_get_latest() {
        let mut book = Book::new();
        assert!(book.is_empty());
        book.update(quote("2330", 58000));
        book.update(quote("2317", 11000));
        book.update(quote("2330", 58100));

        assert_eq!(book.len(), 2);
        assert_eq!(book.get(0, "2330").unwrap().last_price, 58100);
        assert_eq!(book.get(0, "2317").unwrap().last_price, 11000);
        assert!(book.get(0, "9999").is_none());
    }

    #[test]
    fn two_sources_same_symbol_never_clobber() {
        let mut book = Book::new();
        book.update(quote_at("2330", 0, 1, 1, 58000));
        book.update(quote_at("2330", 1, 1, 1, 59000));
        assert_eq!(book.len(), 2);
        assert_eq!(book.get(0, "2330").unwrap().last_price, 58000);
        assert_eq!(book.get(1, "2330").unwrap().last_price, 59000);
        // A newer tick on source 1 leaves source 0 untouched.
        book.update(quote_at("2330", 1, 1, 2, 59100));
        assert_eq!(book.get(0, "2330").unwrap().last_price, 58000);
        assert_eq!(book.get(1, "2330").unwrap().last_price, 59100);
    }

    #[test]
    fn backwards_seq_is_dropped() {
        let mut book = Book::new();
        assert_eq!(
            book.update(quote_at("2330", 0, 1, 5, 58000)),
            Admit::Admitted
        );
        assert_eq!(
            book.update(quote_at("2330", 0, 1, 4, 57000)),
            Admit::Dropped
        );
        assert_eq!(book.get(0, "2330").unwrap().last_price, 58000);
    }

    #[test]
    fn equal_seq_is_admitted_idempotent() {
        let mut book = Book::new();
        assert_eq!(
            book.update(quote_at("2330", 0, 1, 5, 58000)),
            Admit::Admitted
        );
        assert_eq!(
            book.update(quote_at("2330", 0, 1, 5, 58200)),
            Admit::Admitted
        );
        assert_eq!(book.get(0, "2330").unwrap().last_price, 58200);
    }

    #[test]
    fn higher_epoch_lower_seq_is_admitted() {
        // A sidecar restart bumps epoch and resets seq; the higher epoch is newer
        // even though seq went backwards.
        let mut book = Book::new();
        assert_eq!(
            book.update(quote_at("2330", 0, 1, 900, 58000)),
            Admit::Admitted
        );
        assert_eq!(
            book.update(quote_at("2330", 0, 2, 0, 57000)),
            Admit::Admitted
        );
        assert_eq!(book.get(0, "2330").unwrap().last_price, 57000);
    }

    #[test]
    fn legacy_zero_epoch_seq_always_admitted() {
        let mut book = Book::new();
        assert_eq!(book.update(quote("2330", 58000)), Admit::Admitted);
        assert_eq!(book.update(quote("2330", 57000)), Admit::Admitted);
        assert_eq!(book.get(0, "2330").unwrap().last_price, 57000);
    }
}
