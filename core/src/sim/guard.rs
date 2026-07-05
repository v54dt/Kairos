//! The isolation guard — the headline correctness property. kairos-sim must be
//! structurally incapable of touching the live pipeline: it refuses to start if
//! any resolved sim path canonicalizes onto the LIVE Aeron dir or either LIVE
//! socket. Comparison reuses `refuses_live_dir`'s canonical-identity check, so
//! symlinks and non-canonical spellings (`/x/.`, `/y/../x`, trailing slash) can't
//! slip a sim path onto the live feed.

use crate::replay::marker::refuses_live_dir;
use crate::sim::paths::SimPaths;

/// A live target the sim must never coincide with.
struct Live<'a> {
    label: &'a str,
    path: &'a str,
}

/// Verify the sim namespace is disjoint from the live pipeline. `live_aeron` is
/// the live stack's Aeron dir (`effective_stack_dir(None)`), `live_quote`/`live_order`
/// the live sockets. Returns an error naming the first colliding pair; the caller
/// exits non-zero before spawning anything.
pub fn ensure_isolated(
    sim: &SimPaths,
    live_aeron: Option<&str>,
    live_quote: &str,
    live_order: &str,
) -> anyhow::Result<()> {
    if refuses_live_dir(&sim.aeron_dir, live_aeron, false) {
        anyhow::bail!(
            "sim Aeron dir {} is the LIVE stack's Aeron dir; refusing to start (would publish \
             onto the live feed). Set $KAIROS_SIM_AERON_DIR to a separate dir",
            sim.aeron_dir
        );
    }

    let live_socks = [
        Live {
            label: "live quote socket",
            path: live_quote,
        },
        Live {
            label: "live order socket",
            path: live_order,
        },
    ];
    let sim_socks = [
        ("sim quote socket", &sim.quote_sock),
        ("sim order socket", &sim.order_sock),
    ];
    for (sim_label, sim_path) in sim_socks {
        for live in &live_socks {
            if refuses_live_dir(sim_path, Some(live.path), false) {
                anyhow::bail!(
                    "{sim_label} {sim_path} is the {} ({}); refusing to start (a sim trader would \
                     hit the live path). Use a separate sim socket",
                    live.label,
                    live.path
                );
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sim(a: &str, q: &str, o: &str) -> SimPaths {
        SimPaths {
            aeron_dir: a.to_owned(),
            quote_sock: q.to_owned(),
            order_sock: o.to_owned(),
        }
    }

    const LA: &str = "/dev/shm/aeron-bob";
    const LQ: &str = "/run/user/1001/kairos-quotes.sock";
    const LO: &str = "/run/user/1001/kairos-orders.sock";

    #[test]
    fn accepts_when_all_distinct() {
        let s = sim(
            "/dev/shm/aeron-bob-sim",
            "/run/user/1001/kairos-sim-quotes.sock",
            "/run/user/1001/kairos-sim-orders.sock",
        );
        assert!(ensure_isolated(&s, Some(LA), LQ, LO).is_ok());
    }

    #[test]
    fn rejects_when_sim_aeron_is_live() {
        let s = sim(LA, "/x/q.sock", "/x/o.sock");
        assert!(ensure_isolated(&s, Some(LA), LQ, LO).is_err());
    }

    #[test]
    fn rejects_when_sim_quote_is_live_quote() {
        let s = sim("/dev/shm/aeron-bob-sim", LQ, "/x/o.sock");
        assert!(ensure_isolated(&s, Some(LA), LQ, LO).is_err());
    }

    #[test]
    fn rejects_when_sim_order_is_live_order() {
        let s = sim("/dev/shm/aeron-bob-sim", "/x/q.sock", LO);
        assert!(ensure_isolated(&s, Some(LA), LQ, LO).is_err());
    }

    #[test]
    fn rejects_when_sim_socket_hits_the_other_live_socket() {
        // sim quote onto live ORDER, sim order onto live QUOTE.
        let s = sim("/dev/shm/aeron-bob-sim", LO, LQ);
        assert!(ensure_isolated(&s, Some(LA), LQ, LO).is_err());
    }

    #[test]
    fn rejects_non_canonical_spelling_of_live() {
        let s = sim("/dev/shm/aeron-bob/", "/x/q.sock", "/x/o.sock");
        assert!(ensure_isolated(&s, Some(LA), LQ, LO).is_err());
        let s = sim("/dev/shm/foo/../aeron-bob", "/x/q.sock", "/x/o.sock");
        assert!(ensure_isolated(&s, Some(LA), LQ, LO).is_err());
    }

    #[test]
    fn accepts_when_live_aeron_unknown() {
        let s = sim("/dev/shm/aeron-bob-sim", "/x/q.sock", "/x/o.sock");
        assert!(ensure_isolated(&s, None, LQ, LO).is_ok());
    }
}
