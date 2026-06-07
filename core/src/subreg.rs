use std::collections::BTreeMap;

/// Tracks how many UDS clients want each symbol so the hub can publish the
/// union of demanded symbols upstream to the sidecar (control stream 1002).
///
/// The warm set is owned by the sidecar (its own config), so this registry only
/// carries dynamic, client-driven demand: the published desired set is exactly
/// the symbols with at least one live subscriber.
#[derive(Default)]
pub struct SubRegistry {
    counts: BTreeMap<String, usize>,
}

impl SubRegistry {
    pub fn new() -> Self {
        Self::default()
    }

    /// Increment the refcount for `symbol`. Returns true when it entered the
    /// desired set (0 -> 1), i.e. the desired set changed.
    pub fn add(&mut self, symbol: &str) -> bool {
        let count = self.counts.entry(symbol.to_owned()).or_insert(0);
        *count += 1;
        *count == 1
    }

    /// Decrement the refcount for `symbol`. Returns true when it left the
    /// desired set (1 -> 0). Unknown symbols are a no-op.
    pub fn remove(&mut self, symbol: &str) -> bool {
        let Some(&count) = self.counts.get(symbol) else {
            return false;
        };
        if count <= 1 {
            self.counts.remove(symbol);
            true
        } else {
            self.counts.insert(symbol.to_owned(), count - 1);
            false
        }
    }

    /// The current desired set: every symbol with at least one subscriber, in
    /// sorted order (BTreeMap) for a stable wire payload.
    pub fn desired(&self) -> Vec<String> {
        self.counts.keys().cloned().collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn add_enters_desired_set_only_on_first_ref() {
        let mut r = SubRegistry::new();
        assert!(r.add("2330")); // 0 -> 1 changed
        assert!(!r.add("2330")); // 1 -> 2 no change
        assert_eq!(r.desired(), vec!["2330".to_string()]);
    }

    #[test]
    fn remove_leaves_only_when_last_ref_gone() {
        let mut r = SubRegistry::new();
        r.add("2330");
        r.add("2330");
        assert!(!r.remove("2330")); // 2 -> 1 no change
        assert!(r.remove("2330")); // 1 -> 0 changed
        assert!(r.desired().is_empty());
    }

    #[test]
    fn remove_unknown_symbol_is_noop() {
        let mut r = SubRegistry::new();
        assert!(!r.remove("9999"));
    }

    #[test]
    fn desired_is_sorted() {
        let mut r = SubRegistry::new();
        r.add("2330");
        r.add("0050");
        r.add("2317");
        assert_eq!(
            r.desired(),
            vec!["0050".to_string(), "2317".to_string(), "2330".to_string()]
        );
    }

    #[test]
    fn refcount_is_shared_across_clients() {
        let mut r = SubRegistry::new();
        assert!(r.add("2330")); // client A: 0 -> 1
        assert!(!r.add("2330")); // client B: 1 -> 2
        assert!(!r.remove("2330")); // A leaves: 2 -> 1, still wanted
        assert_eq!(r.desired(), vec!["2330".to_string()]);
        assert!(r.remove("2330")); // B leaves: 1 -> 0
        assert!(r.desired().is_empty());
    }
}
