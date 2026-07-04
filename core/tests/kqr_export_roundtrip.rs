//! End-to-end A5 conversion on a synthetic KQR fixture. Builds a KQR file with the
//! record writer (quotes + trades + an unknown-variant control frame, then a torn
//! tail), runs `convert`, and asserts the tallies and spot values in BOTH the
//! Parquet tables and the hftbacktest `.npy` array. All paths are tempdirs — the
//! production archive under /home/coder/Kairos/data is never touched.

use std::fs::File;
use std::path::{Path, PathBuf};

use arrow::array::{Array, Int64Array, StringArray};
use kairos_core::encode::{encode_error, encode_quote, encode_trade};
use kairos_core::export::convert::{convert, existing_outputs};
use kairos_core::model::{Exchange, PriceLevel, Quote, QuoteBoard, Session, Trade};
use kairos_core::record::{FileHeader, RecordWriter};
use kairos_core::replay::KqrSource;
use parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder;

const DAY: &str = "20260704";

fn quote(symbol: &str, quote_ts_us: i64, bid_px: i64) -> Quote {
    Quote {
        symbol: symbol.to_owned(),
        exchange: Exchange::Twse,
        quote_ts_us,
        bids: vec![
            PriceLevel {
                price_mantissa: bid_px,
                price_scale: 2,
                volume: 100,
            },
            PriceLevel {
                price_mantissa: bid_px - 50,
                price_scale: 2,
                volume: 40,
            },
        ],
        asks: vec![PriceLevel {
            price_mantissa: bid_px + 100,
            price_scale: 2,
            volume: 80,
        }],
        last_price: bid_px + 50,
        last_scale: 2,
        last_volume: 10,
        is_trial: false,
        source: 0,
        seq: 1,
        epoch: 1,
        recv_ts_us: quote_ts_us + 5,
        board: QuoteBoard::RoundLot,
        session: Session::Day,
        trading_date: 20_260_704,
        simtrade: false,
        underlying_price: 0,
    }
}

fn trade(symbol: &str, trade_ts_us: i64, px: i64, vol: i64) -> Trade {
    Trade {
        symbol: symbol.to_owned(),
        exchange: Exchange::Twse,
        source: 0,
        seq: 2,
        epoch: 1,
        trade_ts_us,
        recv_ts_us: trade_ts_us + 3,
        price_mantissa: px,
        price_scale: 2,
        volume: vol,
        is_trial: false,
        session: Session::Day,
        trading_date: 20_260_704,
        simtrade: false,
        underlying_price: 0,
    }
}

/// Build a KQR file whose last record is truncated mid-payload (a torn tail).
fn write_fixture(path: &Path) {
    let f = File::create(path).unwrap();
    let mut w = RecordWriter::create(f, &FileHeader::new(1001, 0)).unwrap();
    w.append(1_000, &encode_quote(&quote("2330", 1_000, 58_000)))
        .unwrap();
    w.append(1_010, &encode_quote(&quote("2317", 1_010, 11_000)))
        .unwrap();
    w.append(1_020, &encode_trade(&trade("2330", 1_020, 58_050, 3)))
        .unwrap();
    w.append(1_030, &encode_error("control frame")).unwrap();
    w.append(1_040, &encode_quote(&quote("2330", 1_040, 58_100)))
        .unwrap();
    w.flush().unwrap();
    drop(w);
    // Truncate a few bytes off the end so the last record is torn.
    let data = std::fs::read(path).unwrap();
    std::fs::write(path, &data[..data.len() - 3]).unwrap();
}

fn read_i64(batch: &arrow::array::RecordBatch, col: &str, row: usize) -> i64 {
    batch
        .column_by_name(col)
        .unwrap()
        .as_any()
        .downcast_ref::<Int64Array>()
        .unwrap()
        .value(row)
}

fn tempdir(tag: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("kairos-a5-{}-{}-{tag}", std::process::id(), tag));
    std::fs::create_dir_all(&d).unwrap();
    d
}

#[test]
fn convert_end_to_end_both_outputs() {
    let input = tempdir("in");
    let out = tempdir("out");
    let kqr = input.join(format!("s1001-{DAY}.kqr"));
    write_fixture(&kqr);

    let source = KqrSource::open_files(std::slice::from_ref(&kqr)).unwrap();
    let stats = convert(source, DAY, &out, None).unwrap();

    // 5 records were appended; the last (a 2330 quote) is torn off -> 4 read, 1 warning.
    assert_eq!(stats.records_in, 4);
    assert_eq!(stats.quotes, 2);
    assert_eq!(stats.trades, 1);
    assert_eq!(stats.unknown_variants, 1);
    assert_eq!(stats.decode_errors, 0);
    assert_eq!(stats.frame_warnings, 1);
    assert_eq!(stats.scale_conflicts, 0);

    // --- Parquet quotes ---
    let qfile = File::open(out.join("quotes").join(format!("{DAY}.parquet"))).unwrap();
    let mut qr = ParquetRecordBatchReaderBuilder::try_new(qfile)
        .unwrap()
        .build()
        .unwrap();
    let qbatch = qr.next().unwrap().unwrap();
    assert_eq!(qbatch.num_rows(), 2);
    let sym = qbatch
        .column_by_name("symbol")
        .unwrap()
        .as_any()
        .downcast_ref::<StringArray>()
        .unwrap();
    assert_eq!(sym.value(0), "2330");
    assert_eq!(read_i64(&qbatch, "bid1_px", 0), 58_000);
    assert_eq!(read_i64(&qbatch, "bid2_px", 0), 57_950);
    assert_eq!(read_i64(&qbatch, "ask1_px", 0), 58_100);
    // frame_recv_ts_us is the KQR frame clock; recv_ts_us the sidecar clock.
    assert_eq!(read_i64(&qbatch, "frame_recv_ts_us", 0), 1_000);
    assert_eq!(read_i64(&qbatch, "recv_ts_us", 0), 1_005);

    // --- Parquet trades ---
    let tfile = File::open(out.join("trades").join(format!("{DAY}.parquet"))).unwrap();
    let mut tr = ParquetRecordBatchReaderBuilder::try_new(tfile)
        .unwrap()
        .build()
        .unwrap();
    let tbatch = tr.next().unwrap().unwrap();
    assert_eq!(tbatch.num_rows(), 1);
    assert_eq!(read_i64(&tbatch, "price_mantissa", 0), 58_050);
    assert_eq!(read_i64(&tbatch, "volume", 0), 3);

    // --- hft npy for 2330: 1 surviving quote (clear+2 bids+clear+1 ask = 5) + 1
    // trade = 6 events (the 2330 quote at 1040 was torn off). ---
    let stat = stats.hft_files.iter().find(|f| f.symbol == "2330").unwrap();
    assert_eq!(stat.events, 6);
    let npy = std::fs::read(&stat.path).unwrap();
    // Parse the v1 header, then the fixed 64-byte records.
    let hlen = u16::from_le_bytes([npy[8], npy[9]]) as usize;
    let body = &npy[10 + hlen..];
    assert_eq!(body.len(), 6 * 64);
    // First event is a DEPTH_CLEAR on the bid side; exch_ts = quote_ts * 1000.
    let ev0 = u64::from_le_bytes(body[0..8].try_into().unwrap());
    assert_eq!(ev0, 3 | (1 << 31) | (1 << 30) | (1 << 29));
    let exch0 = i64::from_le_bytes(body[8..16].try_into().unwrap());
    assert_eq!(exch0, 1_000 * 1000);
    // local_ts is the KQR frame recv ts (1000), not the sidecar recv_ts_us (1005).
    let local0 = i64::from_le_bytes(body[16..24].try_into().unwrap());
    assert_eq!(local0, 1_000 * 1000);
    // A trade event (TRADE|EXCH|LOCAL) exists with px 580.50 and qty 3.
    let mut saw_trade = false;
    for i in 0..6 {
        let ev = u64::from_le_bytes(body[i * 64..i * 64 + 8].try_into().unwrap());
        if ev == 2 | (1 << 31) | (1 << 30) {
            let px = f64::from_le_bytes(body[i * 64 + 24..i * 64 + 32].try_into().unwrap());
            let qty = f64::from_le_bytes(body[i * 64 + 32..i * 64 + 40].try_into().unwrap());
            assert_eq!(px, 580.5);
            assert_eq!(qty, 3.0);
            saw_trade = true;
        }
    }
    assert!(saw_trade);

    std::fs::remove_dir_all(&input).ok();
    std::fs::remove_dir_all(&out).ok();
}

#[test]
fn rerun_is_flagged_by_overwrite_guard_and_symbols_filter() {
    let input = tempdir("in2");
    let out = tempdir("out2");
    let kqr = input.join(format!("s1001-{DAY}.kqr"));
    write_fixture(&kqr);

    // Filter to a single symbol.
    let filter: std::collections::HashSet<String> = ["2330".to_owned()].into_iter().collect();
    let source = KqrSource::open_files(std::slice::from_ref(&kqr)).unwrap();
    let stats = convert(source, DAY, &out, Some(&filter)).unwrap();
    assert_eq!(stats.quotes, 1);
    assert_eq!(stats.trades, 1);
    assert!(stats.hft_files.iter().all(|f| f.symbol == "2330"));

    // The overwrite guard now reports the just-written outputs (the bin refuses).
    let existing = existing_outputs(&out, DAY);
    assert!(
        existing
            .iter()
            .any(|p| p.ends_with(format!("{DAY}.parquet")))
    );
    assert!(
        existing
            .iter()
            .any(|p| p.to_string_lossy().ends_with(&format!("2330_{DAY}_v1.npy")))
    );

    // A forced re-run (convert overwrites) succeeds and is deterministic for npy.
    let before = std::fs::read(out.join("hft").join(format!("2330_{DAY}_v1.npy"))).unwrap();
    let source = KqrSource::open_files(&[kqr]).unwrap();
    convert(source, DAY, &out, Some(&filter)).unwrap();
    let after = std::fs::read(out.join("hft").join(format!("2330_{DAY}_v1.npy"))).unwrap();
    assert_eq!(before, after);

    std::fs::remove_dir_all(&input).ok();
    std::fs::remove_dir_all(&out).ok();
}
