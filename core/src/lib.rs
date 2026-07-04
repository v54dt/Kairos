pub mod kairos_capnp {
    include!(concat!(env!("OUT_DIR"), "/kairos_capnp.rs"));
}

pub mod book;
pub mod compare;
pub mod decode;
pub mod encode;
pub mod ipc;
pub mod metrics;
pub mod model;
pub mod record;
pub mod replay;
pub mod subreg;
pub mod uds;
