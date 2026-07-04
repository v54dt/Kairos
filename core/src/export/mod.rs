//! A5 KQR export: a one-way, re-runnable converter from raw KQR capnp logs into
//! research-friendly derived outputs. The raw KQR archive stays the source of
//! truth; this module only ever READS it and writes new files elsewhere.
//!
//! It reuses the shared record/decode/replay-source stack (`KqrSource` ->
//! `decode_feed_event`) — it never reimplements the reader. Two outputs are
//! produced:
//!  1. Parquet derived tables (`quotes`, `trades`) for kairos-lab (polars/duckdb).
//!  2. per-(symbol, day) hftbacktest event arrays as `.npy` for I4 parameter scans.
//!
//! Unknown/future envelope variants and decode errors are counted and skipped,
//! never aborting a file (consistent with the A2 metrics philosophy).
//!
//! # Parquet schema (STABLE — kairos-lab may rely on this)
//!
//! Prices are kept as exact integer mantissa + a `*_scale` column (no float, no
//! precision loss): a value is `mantissa * 10^-scale`. Two receive clocks are
//! disambiguated by giving BOTH columns: `frame_recv_ts_us` is the KQR record
//! header time (recorder clock), `recv_ts_us` is the sidecar clock inside the
//! capnp payload (`0` on legacy quotes that predate the field). All `*_ts_us`
//! columns are microseconds.
//!
//! `quotes` table, one row per Quote:
//! - `frame_recv_ts_us` i64, `recv_ts_us` i64, `quote_ts_us` i64
//! - `symbol` Utf8, `exchange` Utf8 (`twse`/`tpex`/`tfx`/`otc`), `source` u16
//! - `seq` u64, `epoch` u32
//! - `board` Utf8 (`unknown`/`round_lot`/`odd_lot`), `session` Utf8
//!   (`unknown`/`day`/`night`), `is_trial` bool, `simtrade` bool
//! - `trading_date` u32, `underlying_price` i64
//! - `price_scale` u8 (one depth scale per quote; see the scale-conflict note),
//!   `n_bids` u8, `n_asks` u8 (populated-level counts; distinguish a real 0 px/vol
//!   from an absent padding level)
//! - `bid1_px..bid5_px` i64, `bid1_vol..bid5_vol` i64,
//!   `ask1_px..ask5_px` i64, `ask1_vol..ask5_vol` i64 (raw mantissa; absent
//!   levels are 0/0; bids/asks pad/truncate to exactly 5 levels)
//! - `last_price` i64, `last_scale` u8, `last_volume` i64
//!
//! `trades` table, one row per Trade:
//! - `frame_recv_ts_us` i64, `recv_ts_us` i64, `trade_ts_us` i64
//! - `symbol` Utf8, `exchange` Utf8, `source` u16, `seq` u64, `epoch` u32
//! - `price_mantissa` i64, `price_scale` u8, `volume` i64
//! - `is_trial` bool, `session` Utf8, `simtrade` bool
//! - `trading_date` u32, `underlying_price` i64
//!
//! Scale conflict: `price_scale` is a single per-quote column. If a quote's
//! populated depth levels do not all share one scale (never observed on TWSE),
//! the first populated level's scale is written, the raw per-level mantissas are
//! still exact, and the event is counted in `scale_conflicts` and surfaced in the
//! CLI summary. The hftbacktest `px` uses each level's own scale, so it is
//! unaffected.
//!
//! # hftbacktest event layout (`_v1`)
//!
//! The filename is versioned (`<symbol>_<day>_v1.npy`) so a layout revision is
//! additive. Parts are labelled KNOWN vs ASSUMPTION because I4 is not yet built
//! to cross-check the exact conventions.
//!
//! KNOWN (matches nkaz001/hftbacktest v2 event dtype): a 64-byte C-order
//! structured record, 8 little-endian fields in this order —
//! `ev` u8/uint64, `exch_ts` i8/int64, `local_ts` i8/int64, `px` f8/float64,
//! `qty` f8/float64, `order_id` u8/uint64, `ival` i8/int64, `fval` f8/float64.
//! Event-type low bits: `DEPTH_EVENT=1`, `TRADE_EVENT=2`, `DEPTH_CLEAR_EVENT=3`,
//! `DEPTH_SNAPSHOT_EVENT=4`. Flag high bits OR'd in: `EXCH_EVENT=1<<31`,
//! `LOCAL_EVENT=1<<30`, `BUY_EVENT=1<<29`, `SELL_EVENT=1<<28`. Timestamps are
//! NANOSECONDS.
//!
//! MAPPING (KNOWN): `exch_ts = quote_ts_us/trade_ts_us * 1000`,
//! `local_ts = KQR frame recv_ts_us * 1000`, `px = mantissa * 10^-scale` (per
//! level), `qty = volume`. `order_id`/`ival`/`fval` are 0.
//!
//! Quote -> snapshot block (ASSUMPTION, medium confidence): emit a
//! `DEPTH_CLEAR_EVENT | BUY_EVENT` with `px = 0` (full bid-side reset) then one
//! `DEPTH_SNAPSHOT_EVENT | BUY_EVENT` per populated bid level; likewise a
//! `DEPTH_CLEAR_EVENT | SELL_EVENT` + `DEPTH_SNAPSHOT_EVENT | SELL_EVENT` rows for
//! asks. `EXCH_EVENT | LOCAL_EVENT` are set on every row. Padding levels with
//! zero price or zero volume are skipped.
//!
//! Trade -> one `TRADE_EVENT | EXCH_EVENT | LOCAL_EVENT` row (ASSUMPTION: the wire
//! carries no aggressor side, so no BUY/SELL flag is set — queue/fill models that
//! need trade side will not get it in `_v1`).
//!
//! Per symbol, events are written in a total order `(exch_ts, local_ts, seq,
//! insertion_index)` so a re-run is byte-identical. One `.npy` per (symbol, day)
//! is written under `<out>/hft/`; an operator can `np.savez` them into a single
//! `.npz` if a tool ever strictly requires that.

pub mod convert;
pub mod hft;
pub mod npy;
pub mod parquet;
