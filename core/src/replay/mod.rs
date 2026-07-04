//! A3 replay: re-offers recorded KQR fragments onto an (isolated) Aeron stream at
//! one of three paces — realtime, accelerated, or full-speed with a virtual clock.
//!
//! The replay path handles RAW envelope bytes only: it never decodes a capnp
//! payload. Fill simulation (A4) and parquet/npz conversion (A5) live elsewhere.
//! The `QuoteBook::AgeMs` replay-fidelity gap (an engine-attached concern) is
//! deferred to A4; A3 simply preserves recorded bytes and their receive timing.
//!
//! Layout:
//! - `source`: a k-way merge over one or more `.kqr` files, yielding records in
//!   `recv_ts_us` order so interleaved control/data streams replay as recorded.
//! - `pacer`: a pure, clock-injectable iterator that maps each record to the wall
//!   instant it is due, sleeping (or not) accordingly.
//! - `runner`: `drive_replay` glues a paced iterator to an injected offer sink and
//!   maintains the stats counters (shared by the daemon and the integration test).

pub mod marker;
pub mod pacer;
pub mod runner;
pub mod source;

pub use marker::{
    MarkerGuard, default_aeron_dir, ensure_no_active_replay, refuses_live_dir, write_marker,
};
pub use pacer::{Clock, Pace, Paced, Pacer, SystemClock};
pub use runner::{OfferOutcome, ReplayStats, drive_replay};
pub use source::{KqrSource, ReplayRecord};
