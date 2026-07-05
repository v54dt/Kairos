//! Library facade for kairos-top: exposes the data-source layer so integration
//! tests can drive the real quote/feed source against a running sim, without a
//! full-screen terminal. The `kairos-top` binary keeps its own module tree.

pub mod sources;
