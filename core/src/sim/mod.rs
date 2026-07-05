//! kairos-sim: an isolated simulation universe (Aeron dir + quote/order sockets)
//! namespaced so it can never coincide with the live pipeline. The pure,
//! unit-testable pieces live here; the `kairos-sim` binary does the process
//! orchestration and signal handling on top of them.

pub mod cli;
pub mod guard;
pub mod paths;
pub mod proc;

pub use cli::{Command, Opts, parse};
pub use guard::ensure_isolated;
pub use paths::SimPaths;
