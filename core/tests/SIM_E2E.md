# E2E sim drills

Scripted, repeatable end-to-end drills that run the REAL trading path against the
deterministic committed tapes through the ISOLATED sim (own Aeron dir + sim
sockets), off-hours, asserting the full flow and clean teardown. They orchestrate
only existing binaries (`kairos-sim`, `kairos_sim_hubd`, `kairos_scenario_trader`,
the tui feed source) — no behavior is changed.

## Run

Single entry point (builds the release core bins, checks the C++ bins, runs both
drills, prints a PASS/FAIL summary):

    make sim-e2e

The drills are `#[ignore]` integration tests, so they are NOT in the default
`cargo test` CI job. To run them individually:

    # trader fills -> journal -> teardown
    KAIROS_SIM_HUBD=exec/scenario/build/kairos_sim_hubd \
    KAIROS_SCENARIO_TRADER=exec/scenario/build/kairos_scenario_trader \
      cargo test --manifest-path core/Cargo.toml --test sim_e2e -- --ignored --nocapture

    # tui reads sim quotes
    KAIROS_SIM_BIN=core/target/release/kairos-sim \
    KAIROS_SIM_HUBD=exec/scenario/build/kairos_sim_hubd \
      cargo test -p kairos-tui --test sim_quote_smoke -- --ignored --nocapture

Prerequisite binaries (the sim path needs no concords SDK):

    cargo build --release --manifest-path core/Cargo.toml --bins
    cmake -S exec/scenario -B exec/scenario/build -DCMAKE_BUILD_TYPE=Release
    cmake --build exec/scenario/build --target kairos_sim_hubd kairos_scenario_trader

## Drills

### 1. Trader fills -> journal -> teardown (`core/tests/sim_e2e.rs`)

Replays `core/tests/fixtures/tapes/trend_day_2330.kqr` through an isolated sim,
runs the real `kairos_scenario_trader --live` against the sim order/quote sockets,
and asserts:

- (a) the trader FILLS (acks + fills, not acked-but-0-fill),
- (b) the B2 order journal records the fsynced fills,
- (c) the trader ends cleanly on SIGTERM, and
- (d) teardown leaves NO orphan process groups (every sim child pgid returns ESRCH
  and the pidfile is removed; a final `pgrep` confirms no process references the
  drill's temp dir).

The scenario fixture is `core/tests/fixtures/scenarios/trend_day_2330_drill.toml`
(Buy 2330, RoundLot, `shares_per_order = 1000`, `budget_twd = 5000000`,
`pricing.policy = "cross"`). The crossing BUY takes the descending trend-day trades
(the tape prints walk below ~579.50), so the sim hub's conservative fill model
fills deterministically. The harness injects `[journal].dir` (a per-run temp path)
before launching the trader; the committed fixture omits it to avoid a
machine-specific absolute path.

Exact trader invocation used by the drill:

    KAIROS_QUOTE_SOCK=<sim q.sock> KAIROS_ORDER_SOCK=<sim o.sock> \
    KAIROS_BLACKLIST_CSV=<absent path> \
      kairos_scenario_trader <scenario.toml> \
        --live --ignore-window --ignore-blacklist --yes

`--ignore-window` because the tape timestamps are 09:00-13:25 but the drill runs
off-hours; the blacklist gate is decoupled from lab state by pointing
`KAIROS_BLACKLIST_CSV` at an isolated absent path (deterministic refuse) plus
`--ignore-blacklist --yes` (deterministic override).

### 2. TUI reads sim quotes (`tui/tests/sim_quote_smoke.rs`)

Brings up the same isolated sim and drives the REAL
`kairos_tui::sources::feed::run` against the sim quote socket, asserting the feed
source connects and the 2330 book is populated with real quotes (the real-quote
integration coverage the tui unit tests lack — no mocked executor). Then tears the
sim down with the same pgid/ESRCH no-orphan assertion.

The sim hub writes no `kairos-hub-status.json`, so the `hub_status` source has no
sim data source and is out of scope here (N/A, not faked).

## Isolation & determinism

- Pid-tagged temp base `$TMPDIR/kairos-e2e-<pid>` and `/dev/shm/aeron-e2e-<pid>`,
  with `KAIROS_SIM_*` env (never the live `KAIROS_*` names). `kairos-sim`'s
  hard isolation guard refuses to start on the live Aeron dir or sockets.
- The sim spawns `kairos_sim_hubd` WITHOUT `--prob`, so fills are deterministic
  against the fixed tape. Do not enable `--prob` here — it would flake the
  twice-repeatable assertion.
- The harness owns the trader (not under the sim guard), so it SIGTERMs + reaps the
  trader BEFORE tearing the sim down, then verifies every sim child pgid is gone.

## Deferred follow-on stages (next T3 slices, not built here)

- B2 restart-replay drill: take a partial fill, kill the trader mid-run, restart,
  assert the journal replay restores accounting so the budget is not re-bought.
- Chaos-lite: kill core / cut a socket / disk-full during replay, assert graceful
  behavior.
- B5 kill-switch drill.
- E3 night-session reject-repeg gate.
