//! Library facade for kairos-top: exposes the full module tree (data sources,
//! panels, input handling, app state) so integration tests can drive the real
//! quote/feed source and render panels without a full-screen terminal. The
//! `kairos-top` binary keeps only `fn main` and the event loop.

pub mod app;
pub mod format;
pub mod input;
pub mod panels;
pub mod sources;
pub mod terminal;
