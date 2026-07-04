//! Parquet derived tables (`quotes`, `trades`) for kairos-lab. The column
//! contract is documented in `export::mod`; this module owns the arrow schema,
//! the Quote/Trade -> row extraction (including per-quote scale-conflict
//! detection), and a streaming writer that flushes bounded row groups so a full
//! trading day never has to be held in memory at once.

use std::fs::File;
use std::io::BufWriter;
use std::path::Path;
use std::sync::Arc;

use anyhow::Result;
use arrow::array::{
    ArrayRef, BooleanArray, Int64Array, RecordBatch, StringArray, UInt8Array, UInt16Array,
    UInt32Array, UInt64Array,
};
use arrow::datatypes::{DataType, Field, Schema, SchemaRef};
use parquet::arrow::ArrowWriter;
use parquet::basic::{Compression, ZstdLevel};
use parquet::file::properties::WriterProperties;

use crate::model::{Exchange, Quote, QuoteBoard, Session, Trade};

const ROW_GROUP_ROWS: usize = 64 * 1024;
const LEVELS: usize = 5;

fn exchange_str(e: Exchange) -> &'static str {
    match e {
        Exchange::Twse => "twse",
        Exchange::Tpex => "tpex",
        Exchange::Tfx => "tfx",
        Exchange::Otc => "otc",
    }
}

fn board_str(b: QuoteBoard) -> &'static str {
    match b {
        QuoteBoard::Unknown => "unknown",
        QuoteBoard::RoundLot => "round_lot",
        QuoteBoard::OddLot => "odd_lot",
    }
}

fn session_str(s: Session) -> &'static str {
    match s {
        Session::Unknown => "unknown",
        Session::Day => "day",
        Session::Night => "night",
    }
}

fn writer_props() -> WriterProperties {
    WriterProperties::builder()
        .set_compression(Compression::ZSTD(ZstdLevel::default()))
        .build()
}

// --- quotes -----------------------------------------------------------------

/// One extracted quotes-table row (5-level, raw integer mantissas).
#[derive(Clone)]
pub struct QuoteRow {
    pub frame_recv_ts_us: i64,
    pub recv_ts_us: i64,
    pub quote_ts_us: i64,
    pub symbol: String,
    pub exchange: &'static str,
    pub source: u16,
    pub seq: u64,
    pub epoch: u32,
    pub board: &'static str,
    pub session: &'static str,
    pub is_trial: bool,
    pub simtrade: bool,
    pub trading_date: u32,
    pub underlying_price: i64,
    pub price_scale: u8,
    pub n_bids: u8,
    pub n_asks: u8,
    pub bid_px: [i64; LEVELS],
    pub bid_vol: [i64; LEVELS],
    pub ask_px: [i64; LEVELS],
    pub ask_vol: [i64; LEVELS],
    pub last_price: i64,
    pub last_scale: u8,
    pub last_volume: i64,
}

impl QuoteRow {
    /// Extract a row from a Quote. The bool is `true` when the quote's populated
    /// depth levels do not all share one price scale (surfaced as a scale
    /// conflict); the first level's scale is then used for `price_scale` while the
    /// raw per-level mantissas remain exact.
    pub fn from_quote(q: &Quote, frame_recv_ts_us: i64) -> (Self, bool) {
        let mut bid_px = [0i64; LEVELS];
        let mut bid_vol = [0i64; LEVELS];
        for (i, l) in q.bids.iter().take(LEVELS).enumerate() {
            bid_px[i] = l.price_mantissa;
            bid_vol[i] = l.volume;
        }
        let mut ask_px = [0i64; LEVELS];
        let mut ask_vol = [0i64; LEVELS];
        for (i, l) in q.asks.iter().take(LEVELS).enumerate() {
            ask_px[i] = l.price_mantissa;
            ask_vol[i] = l.volume;
        }
        let mut scale: Option<u8> = None;
        let mut conflict = false;
        for l in q.bids.iter().chain(q.asks.iter()) {
            match scale {
                None => scale = Some(l.price_scale),
                Some(s) if s != l.price_scale => conflict = true,
                _ => {}
            }
        }
        let row = QuoteRow {
            frame_recv_ts_us,
            recv_ts_us: q.recv_ts_us,
            quote_ts_us: q.quote_ts_us,
            symbol: q.symbol.clone(),
            exchange: exchange_str(q.exchange),
            source: q.source,
            seq: q.seq,
            epoch: q.epoch,
            board: board_str(q.board),
            session: session_str(q.session),
            is_trial: q.is_trial,
            simtrade: q.simtrade,
            trading_date: q.trading_date,
            underlying_price: q.underlying_price,
            price_scale: scale.unwrap_or(0),
            n_bids: q.bids.len().min(LEVELS) as u8,
            n_asks: q.asks.len().min(LEVELS) as u8,
            bid_px,
            bid_vol,
            ask_px,
            ask_vol,
            last_price: q.last_price,
            last_scale: q.last_scale,
            last_volume: q.last_volume,
        };
        (row, conflict)
    }
}

fn quotes_schema() -> SchemaRef {
    let mut fields = vec![
        Field::new("frame_recv_ts_us", DataType::Int64, false),
        Field::new("recv_ts_us", DataType::Int64, false),
        Field::new("quote_ts_us", DataType::Int64, false),
        Field::new("symbol", DataType::Utf8, false),
        Field::new("exchange", DataType::Utf8, false),
        Field::new("source", DataType::UInt16, false),
        Field::new("seq", DataType::UInt64, false),
        Field::new("epoch", DataType::UInt32, false),
        Field::new("board", DataType::Utf8, false),
        Field::new("session", DataType::Utf8, false),
        Field::new("is_trial", DataType::Boolean, false),
        Field::new("simtrade", DataType::Boolean, false),
        Field::new("trading_date", DataType::UInt32, false),
        Field::new("underlying_price", DataType::Int64, false),
        Field::new("price_scale", DataType::UInt8, false),
        Field::new("n_bids", DataType::UInt8, false),
        Field::new("n_asks", DataType::UInt8, false),
    ];
    for side in ["bid", "ask"] {
        for l in 1..=LEVELS {
            fields.push(Field::new(format!("{side}{l}_px"), DataType::Int64, false));
            fields.push(Field::new(format!("{side}{l}_vol"), DataType::Int64, false));
        }
    }
    fields.push(Field::new("last_price", DataType::Int64, false));
    fields.push(Field::new("last_scale", DataType::UInt8, false));
    fields.push(Field::new("last_volume", DataType::Int64, false));
    Arc::new(Schema::new(fields))
}

fn quotes_batch(schema: &SchemaRef, rows: &[QuoteRow]) -> Result<RecordBatch> {
    let mut cols: Vec<ArrayRef> = vec![
        Arc::new(Int64Array::from_iter_values(
            rows.iter().map(|r| r.frame_recv_ts_us),
        )),
        Arc::new(Int64Array::from_iter_values(
            rows.iter().map(|r| r.recv_ts_us),
        )),
        Arc::new(Int64Array::from_iter_values(
            rows.iter().map(|r| r.quote_ts_us),
        )),
        Arc::new(StringArray::from_iter_values(
            rows.iter().map(|r| r.symbol.as_str()),
        )),
        Arc::new(StringArray::from_iter_values(
            rows.iter().map(|r| r.exchange),
        )),
        Arc::new(UInt16Array::from_iter_values(rows.iter().map(|r| r.source))),
        Arc::new(UInt64Array::from_iter_values(rows.iter().map(|r| r.seq))),
        Arc::new(UInt32Array::from_iter_values(rows.iter().map(|r| r.epoch))),
        Arc::new(StringArray::from_iter_values(rows.iter().map(|r| r.board))),
        Arc::new(StringArray::from_iter_values(
            rows.iter().map(|r| r.session),
        )),
        Arc::new(BooleanArray::from(
            rows.iter().map(|r| r.is_trial).collect::<Vec<bool>>(),
        )),
        Arc::new(BooleanArray::from(
            rows.iter().map(|r| r.simtrade).collect::<Vec<bool>>(),
        )),
        Arc::new(UInt32Array::from_iter_values(
            rows.iter().map(|r| r.trading_date),
        )),
        Arc::new(Int64Array::from_iter_values(
            rows.iter().map(|r| r.underlying_price),
        )),
        Arc::new(UInt8Array::from_iter_values(
            rows.iter().map(|r| r.price_scale),
        )),
        Arc::new(UInt8Array::from_iter_values(rows.iter().map(|r| r.n_bids))),
        Arc::new(UInt8Array::from_iter_values(rows.iter().map(|r| r.n_asks))),
    ];
    for l in 0..LEVELS {
        cols.push(Arc::new(Int64Array::from_iter_values(
            rows.iter().map(|r| r.bid_px[l]),
        )));
        cols.push(Arc::new(Int64Array::from_iter_values(
            rows.iter().map(|r| r.bid_vol[l]),
        )));
    }
    for l in 0..LEVELS {
        cols.push(Arc::new(Int64Array::from_iter_values(
            rows.iter().map(|r| r.ask_px[l]),
        )));
        cols.push(Arc::new(Int64Array::from_iter_values(
            rows.iter().map(|r| r.ask_vol[l]),
        )));
    }
    cols.push(Arc::new(Int64Array::from_iter_values(
        rows.iter().map(|r| r.last_price),
    )));
    cols.push(Arc::new(UInt8Array::from_iter_values(
        rows.iter().map(|r| r.last_scale),
    )));
    cols.push(Arc::new(Int64Array::from_iter_values(
        rows.iter().map(|r| r.last_volume),
    )));
    Ok(RecordBatch::try_new(schema.clone(), cols)?)
}

/// Streaming quotes-table writer: buffers rows and flushes a row group every
/// `ROW_GROUP_ROWS`.
pub struct QuotesWriter {
    inner: ArrowWriter<BufWriter<File>>,
    schema: SchemaRef,
    buf: Vec<QuoteRow>,
    rows: u64,
}

impl QuotesWriter {
    pub fn create(path: &Path) -> Result<Self> {
        let schema = quotes_schema();
        let file = BufWriter::new(File::create(path)?);
        let inner = ArrowWriter::try_new(file, schema.clone(), Some(writer_props()))?;
        Ok(Self {
            inner,
            schema,
            buf: Vec::new(),
            rows: 0,
        })
    }

    pub fn push(&mut self, row: QuoteRow) -> Result<()> {
        self.buf.push(row);
        if self.buf.len() >= ROW_GROUP_ROWS {
            self.flush()?;
        }
        Ok(())
    }

    fn flush(&mut self) -> Result<()> {
        if self.buf.is_empty() {
            return Ok(());
        }
        let batch = quotes_batch(&self.schema, &self.buf)?;
        self.inner.write(&batch)?;
        self.rows += self.buf.len() as u64;
        self.buf.clear();
        Ok(())
    }

    /// Flush the tail and close the file, returning the total rows written.
    pub fn finish(mut self) -> Result<u64> {
        self.flush()?;
        self.inner.close()?;
        Ok(self.rows)
    }
}

// --- trades -----------------------------------------------------------------

/// One extracted trades-table row.
#[derive(Clone)]
pub struct TradeRow {
    pub frame_recv_ts_us: i64,
    pub recv_ts_us: i64,
    pub trade_ts_us: i64,
    pub symbol: String,
    pub exchange: &'static str,
    pub source: u16,
    pub seq: u64,
    pub epoch: u32,
    pub price_mantissa: i64,
    pub price_scale: u8,
    pub volume: i64,
    pub is_trial: bool,
    pub session: &'static str,
    pub simtrade: bool,
    pub trading_date: u32,
    pub underlying_price: i64,
}

impl TradeRow {
    pub fn from_trade(t: &Trade, frame_recv_ts_us: i64) -> Self {
        TradeRow {
            frame_recv_ts_us,
            recv_ts_us: t.recv_ts_us,
            trade_ts_us: t.trade_ts_us,
            symbol: t.symbol.clone(),
            exchange: exchange_str(t.exchange),
            source: t.source,
            seq: t.seq,
            epoch: t.epoch,
            price_mantissa: t.price_mantissa,
            price_scale: t.price_scale,
            volume: t.volume,
            is_trial: t.is_trial,
            session: session_str(t.session),
            simtrade: t.simtrade,
            trading_date: t.trading_date,
            underlying_price: t.underlying_price,
        }
    }
}

fn trades_schema() -> SchemaRef {
    Arc::new(Schema::new(vec![
        Field::new("frame_recv_ts_us", DataType::Int64, false),
        Field::new("recv_ts_us", DataType::Int64, false),
        Field::new("trade_ts_us", DataType::Int64, false),
        Field::new("symbol", DataType::Utf8, false),
        Field::new("exchange", DataType::Utf8, false),
        Field::new("source", DataType::UInt16, false),
        Field::new("seq", DataType::UInt64, false),
        Field::new("epoch", DataType::UInt32, false),
        Field::new("price_mantissa", DataType::Int64, false),
        Field::new("price_scale", DataType::UInt8, false),
        Field::new("volume", DataType::Int64, false),
        Field::new("is_trial", DataType::Boolean, false),
        Field::new("session", DataType::Utf8, false),
        Field::new("simtrade", DataType::Boolean, false),
        Field::new("trading_date", DataType::UInt32, false),
        Field::new("underlying_price", DataType::Int64, false),
    ]))
}

fn trades_batch(schema: &SchemaRef, rows: &[TradeRow]) -> Result<RecordBatch> {
    let cols: Vec<ArrayRef> = vec![
        Arc::new(Int64Array::from_iter_values(
            rows.iter().map(|r| r.frame_recv_ts_us),
        )),
        Arc::new(Int64Array::from_iter_values(
            rows.iter().map(|r| r.recv_ts_us),
        )),
        Arc::new(Int64Array::from_iter_values(
            rows.iter().map(|r| r.trade_ts_us),
        )),
        Arc::new(StringArray::from_iter_values(
            rows.iter().map(|r| r.symbol.as_str()),
        )),
        Arc::new(StringArray::from_iter_values(
            rows.iter().map(|r| r.exchange),
        )),
        Arc::new(UInt16Array::from_iter_values(rows.iter().map(|r| r.source))),
        Arc::new(UInt64Array::from_iter_values(rows.iter().map(|r| r.seq))),
        Arc::new(UInt32Array::from_iter_values(rows.iter().map(|r| r.epoch))),
        Arc::new(Int64Array::from_iter_values(
            rows.iter().map(|r| r.price_mantissa),
        )),
        Arc::new(UInt8Array::from_iter_values(
            rows.iter().map(|r| r.price_scale),
        )),
        Arc::new(Int64Array::from_iter_values(rows.iter().map(|r| r.volume))),
        Arc::new(BooleanArray::from(
            rows.iter().map(|r| r.is_trial).collect::<Vec<bool>>(),
        )),
        Arc::new(StringArray::from_iter_values(
            rows.iter().map(|r| r.session),
        )),
        Arc::new(BooleanArray::from(
            rows.iter().map(|r| r.simtrade).collect::<Vec<bool>>(),
        )),
        Arc::new(UInt32Array::from_iter_values(
            rows.iter().map(|r| r.trading_date),
        )),
        Arc::new(Int64Array::from_iter_values(
            rows.iter().map(|r| r.underlying_price),
        )),
    ];
    Ok(RecordBatch::try_new(schema.clone(), cols)?)
}

/// Streaming trades-table writer.
pub struct TradesWriter {
    inner: ArrowWriter<BufWriter<File>>,
    schema: SchemaRef,
    buf: Vec<TradeRow>,
    rows: u64,
}

impl TradesWriter {
    pub fn create(path: &Path) -> Result<Self> {
        let schema = trades_schema();
        let file = BufWriter::new(File::create(path)?);
        let inner = ArrowWriter::try_new(file, schema.clone(), Some(writer_props()))?;
        Ok(Self {
            inner,
            schema,
            buf: Vec::new(),
            rows: 0,
        })
    }

    pub fn push(&mut self, row: TradeRow) -> Result<()> {
        self.buf.push(row);
        if self.buf.len() >= ROW_GROUP_ROWS {
            self.flush()?;
        }
        Ok(())
    }

    fn flush(&mut self) -> Result<()> {
        if self.buf.is_empty() {
            return Ok(());
        }
        let batch = trades_batch(&self.schema, &self.buf)?;
        self.inner.write(&batch)?;
        self.rows += self.buf.len() as u64;
        self.buf.clear();
        Ok(())
    }

    pub fn finish(mut self) -> Result<u64> {
        self.flush()?;
        self.inner.close()?;
        Ok(self.rows)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{Exchange, PriceLevel, QuoteBoard};
    use arrow::array::{Array, Int64Array, StringArray, UInt8Array};
    use parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder;

    fn tmp(name: &str) -> std::path::PathBuf {
        std::env::temp_dir().join(format!("kairos-parquet-{}-{name}", std::process::id()))
    }

    fn sample_quote() -> Quote {
        Quote {
            symbol: "2330".to_owned(),
            exchange: Exchange::Twse,
            quote_ts_us: 1_700_000_000_000_000,
            bids: vec![PriceLevel {
                price_mantissa: 58000,
                price_scale: 2,
                volume: 100,
            }],
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
            seq: 7,
            epoch: 1,
            recv_ts_us: 1_700_000_000_000_050,
            board: QuoteBoard::RoundLot,
            session: Session::Day,
            trading_date: 20_260_704,
            simtrade: false,
            underlying_price: 0,
        }
    }

    fn sample_trade() -> Trade {
        Trade {
            symbol: "2317".to_owned(),
            exchange: Exchange::Twse,
            source: 0,
            seq: 9,
            epoch: 1,
            trade_ts_us: 1_700_000_000_000_100,
            recv_ts_us: 1_700_000_000_000_101,
            price_mantissa: 11050,
            price_scale: 2,
            volume: 3,
            is_trial: false,
            session: Session::Day,
            trading_date: 20_260_704,
            simtrade: false,
            underlying_price: 0,
        }
    }

    #[test]
    fn quotes_write_read_round_trip() {
        let path = tmp("quotes.parquet");
        let (row, conflict) = QuoteRow::from_quote(&sample_quote(), 1_700_000_000_000_060);
        assert!(!conflict);
        let mut w = QuotesWriter::create(&path).unwrap();
        w.push(row).unwrap();
        assert_eq!(w.finish().unwrap(), 1);

        let file = File::open(&path).unwrap();
        let mut reader = ParquetRecordBatchReaderBuilder::try_new(file)
            .unwrap()
            .build()
            .unwrap();
        let batch = reader.next().unwrap().unwrap();
        assert_eq!(batch.num_rows(), 1);

        let sym = batch
            .column_by_name("symbol")
            .unwrap()
            .as_any()
            .downcast_ref::<StringArray>()
            .unwrap();
        assert_eq!(sym.value(0), "2330");

        let bid1 = batch
            .column_by_name("bid1_px")
            .unwrap()
            .as_any()
            .downcast_ref::<Int64Array>()
            .unwrap();
        assert_eq!(bid1.value(0), 58000);

        let scale = batch
            .column_by_name("price_scale")
            .unwrap()
            .as_any()
            .downcast_ref::<UInt8Array>()
            .unwrap();
        assert_eq!(scale.value(0), 2);

        let frame = batch
            .column_by_name("frame_recv_ts_us")
            .unwrap()
            .as_any()
            .downcast_ref::<Int64Array>()
            .unwrap();
        assert_eq!(frame.value(0), 1_700_000_000_000_060);
        let recv = batch
            .column_by_name("recv_ts_us")
            .unwrap()
            .as_any()
            .downcast_ref::<Int64Array>()
            .unwrap();
        assert_eq!(recv.value(0), 1_700_000_000_000_050);

        std::fs::remove_file(&path).ok();
    }

    #[test]
    fn trades_write_read_round_trip() {
        let path = tmp("trades.parquet");
        let mut w = TradesWriter::create(&path).unwrap();
        w.push(TradeRow::from_trade(&sample_trade(), 1_700_000_000_000_110))
            .unwrap();
        assert_eq!(w.finish().unwrap(), 1);

        let file = File::open(&path).unwrap();
        let mut reader = ParquetRecordBatchReaderBuilder::try_new(file)
            .unwrap()
            .build()
            .unwrap();
        let batch = reader.next().unwrap().unwrap();
        let px = batch
            .column_by_name("price_mantissa")
            .unwrap()
            .as_any()
            .downcast_ref::<Int64Array>()
            .unwrap();
        assert_eq!(px.value(0), 11050);
        let vol = batch
            .column_by_name("volume")
            .unwrap()
            .as_any()
            .downcast_ref::<Int64Array>()
            .unwrap();
        assert_eq!(vol.value(0), 3);

        std::fs::remove_file(&path).ok();
    }

    #[test]
    fn mixed_scale_flags_conflict() {
        let mut q = sample_quote();
        q.asks[0].price_scale = 1;
        let (_, conflict) = QuoteRow::from_quote(&q, 0);
        assert!(conflict);
    }
}
