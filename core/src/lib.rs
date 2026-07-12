pub mod kairos_capnp {
    include!(concat!(env!("OUT_DIR"), "/kairos_capnp.rs"));
}

pub mod book;
pub mod compare;
pub mod config;
pub mod daemon;
pub mod decode;
pub mod encode;
pub mod export;
pub mod failover;
pub mod ipc;
pub mod lat_hist;
pub mod metrics;
pub mod model;
pub mod poll;
pub mod record;
pub mod replay;
pub mod shutdown;
pub mod sim;
pub mod streams;
pub mod subreg;
pub mod tapegen;
pub mod uds;
pub mod watchdog;
