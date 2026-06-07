pub mod kairos_capnp {
    include!(concat!(env!("OUT_DIR"), "/kairos_capnp.rs"));
}

pub mod book;
pub mod decode;
pub mod encode;
pub mod ipc;
pub mod model;
