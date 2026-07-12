//! One shutdown signal with both faces the core needs: a sync `AtomicBool` the poll
//! and failover threads read, and an async `watch` the UDS server selects on.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use tokio::sync::watch;

/// A unified SIGTERM/SIGINT signal. `set` stores the sync flag BEFORE sending the
/// watch, so any task woken by the watch is guaranteed to also observe `is_set()`;
/// that ordering is what lets a driver-timeout race during teardown resolve to a
/// clean stop (exit 0) instead of a spurious FATAL restart.
#[derive(Clone)]
pub struct Shutdown {
    flag: Arc<AtomicBool>,
    tx: watch::Sender<bool>,
}

impl Shutdown {
    pub fn new() -> Self {
        let (tx, _rx) = watch::channel(false);
        Self {
            flag: Arc::new(AtomicBool::new(false)),
            tx,
        }
    }

    /// Flip the signal: sync flag first, then wake every async watcher.
    /// `send_replace` stores the value even with zero receivers, so a signal that
    /// fires before any task subscribes is not lost.
    pub fn set(&self) {
        self.flag.store(true, Ordering::SeqCst);
        self.tx.send_replace(true);
    }

    /// Sync read for the poll/failover threads.
    pub fn is_set(&self) -> bool {
        self.flag.load(Ordering::SeqCst)
    }

    /// A fresh async receiver whose `changed()` fires when `set()` is called.
    /// If the signal is already set, mark the receiver changed so a subscriber
    /// created after `set()` still observes the shutdown instead of blocking.
    pub fn subscribe(&self) -> watch::Receiver<bool> {
        let mut rx = self.tx.subscribe();
        if self.is_set() {
            rx.mark_changed();
        }
        rx
    }
}

impl Default for Shutdown {
    fn default() -> Self {
        Self::new()
    }
}
