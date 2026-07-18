# Kairos operator runbook

The single operator reference for running and diagnosing the Kairos pipeline
(Rust `core/`, C++ `exec/scenario/` + `sidecar/concords/`, Rust `tui/`). Every
fact here is taken from the source; the env table is guarded by the honesty test
`core/tests/runbook_census.rs` (a `KAIROS_*` var in source but absent here fails
CI).

Conventions used below:
- **env > XDG > run-user** means: the named env var wins; else
  `$XDG_RUNTIME_DIR/<name>`; else `/run/user/<uid>/<name>` (the per-user `0700`
  dir), only if it exists. Never `/tmp` (world-writable). Absent all three the
  process exits with a `set $<VAR> or $XDG_RUNTIME_DIR` message.
- **toml > env > default** means the config file key wins over the env var.

## 1. Environment variables

All runtime `KAIROS_*` variables read by the shipped binaries. "Component" is the
process that reads it. Precedence is stated per row; a blank/empty value is
treated as unset everywhere.

| Variable | Read by | Purpose | Default | Precedence |
| --- | --- | --- | --- | --- |
| `KAIROS_AERON_DIR` | core, driver, recordd, replayd | Aeron media-driver shared-memory (CnC) dir; point driver+core+replay at one isolated dir | `$AERON_DIR`, else `/dev/shm/aeron-<user>` (Aeron's own default) | `KAIROS_AERON_DIR` > `AERON_DIR` > default. Sidecar uses `sidecar.toml [aeron] dir` instead |
| `KAIROS_AERON_MAX_POLL_ERRORS` | core (watchdog) | Consecutive Aeron poll-error budget before core exits nonzero | `64` | env > default; non-numeric or `0` falls back to default |
| `KAIROS_DRIVER_TIMEOUT_MS` | core (watchdog) | Driver heartbeat-staleness threshold (ms); core exits nonzero if the driver is silent this long | `10000` | env > default; non-numeric or non-positive falls back |
| `KAIROS_FAILOVER_STALE_MS` | core (failover) | A quotes source is stale after this long without a quote, triggering failover to the next-priority source | `2000` | env > default |
| `KAIROS_FAILOVER_RECOVER_HOLD_MS` | core (failover) | How long a recovered higher-priority source must stay fresh before failing back to it | `5000` | env > default |
| `KAIROS_STREAMS` | core | Stream table `id:source[:role]` (comma-separated); ids unique, each quotes source unique | quotes `1001` source `0` + control `1002` source `0` | env (non-empty) > default |
| `KAIROS_SOURCE_PRIORITY` | core | Failover source-priority order (comma list of source ids); must cover every configured quotes source | quotes sources in declared order | env > default |
| `KAIROS_QUOTE_SOCK` | core, exec (trader/hub), tui | Quote UDS: core publishes, consumers read | `<runtime>/kairos-quotes.sock` | env > XDG > run-user |
| `KAIROS_ORDER_SOCK` | core (resolver), exec hub + scenarios | Order-hub UDS (hub <-> scenarios; not core) | `<runtime>/kairos-orders.sock` | env > XDG > run-user; or `hub.toml [hub] socket_path` |
| `KAIROS_SCENARIO_CTL_SOCK` | exec supervisor, tui | Supervisor control UDS (TUI/operator <-> supervisor) | `<runtime>/kairos-scenario-ctl.sock` | env > XDG > run-user; or `--ctl-sock` flag |
| `KAIROS_SIGNAL_SOCK` | exec signal client (B7; daemon in a later PR) | Signal daemon UDS (signald pushes signals/heartbeats to armed traders) | `<runtime>/kairos-signals.sock` | env > XDG > run-user |
| `KAIROS_HUB_STATUS` | exec hub (writes), tui (reads) | Hub status JSON path (fields incl. `halted`) | `<runtime>/kairos-hub-status.json` | env > XDG > run-user |
| `KAIROS_HUB_HALT` | exec hub (watches), tui (arms/clears) | Admin-halt sentinel file; its existence halts all new submits | `<runtime>/kairos-hub-halt` | env > XDG > run-user |
| `KAIROS_JOURNAL_DIR` | exec trader + hub | Shared run-state journal dir; same-day fills replay on restart so budget is not re-bought and hub unroutable fills land beside the trader's | `$HOME/Kairos/data/journal` (trader live: `<data-dir>/journal`) | `[journal].dir` / `[hub].journal_dir` toml > `KAIROS_JOURNAL_DIR` > legacy per-side env > `$HOME/Kairos/data/journal` |
| `KAIROS_HUB_JOURNAL_DIR` | exec hub | **Deprecated** legacy fallback for the hub journal dir; logs a deprecation note | (none) | `[hub].journal_dir` > `KAIROS_JOURNAL_DIR` > `KAIROS_HUB_JOURNAL_DIR` (deprecated) > `$HOME/Kairos/data/journal`. Prefer `KAIROS_JOURNAL_DIR` |
| `KAIROS_BLACKLIST_CSV` | exec trader, tui | Path to the blacklist CSV (suspension/disposal/etc.) | `/home/coder/kairos-lab/data/blacklist/current.csv` | config_path/flag > env > lab default |
| `KAIROS_SCENARIO_DIR` | exec supervisor | Dir the supervisor scans for scenario tomls | `$HOME/Kairos/exec/scenario` | `--scenario-dir` flag > env > default |
| `KAIROS_SCENARIO_TRADER` | exec supervisor; core sim tests | Path to the `kairos_scenario_trader` binary | `<scenario_dir>/build/kairos_scenario_trader` | `--trader-bin` flag > env > default |
| `KAIROS_RESTART_BASE_MS` | exec supervisor | Crash-restart backoff base delay (ms); doubles each retry up to max | `1000` | env override, else `RestartPolicy` default |
| `KAIROS_RESTART_MAX_MS` | exec supervisor | Crash-restart backoff cap (ms) | `60000` | env override, else default |
| `KAIROS_RESTART_MAX_RETRIES` | exec supervisor | Consecutive crash restarts before the supervisor gives up on a scenario; the counter is cleared only by an operator start (a fail-closed halt, exit 17/2, never restarts at all) | `5` | env override, else default |
| `KAIROS_SIM_AERON_DIR` | core (kairos-sim) | Isolated sim Aeron dir (never collides with live) | `/dev/shm/aeron-<user>-sim` | `--aeron-dir` flag > env > namespaced default |
| `KAIROS_SIM_QUOTE_SOCK` | core (kairos-sim) | Isolated sim quote UDS | `<runtime>/kairos-sim-quotes.sock` | `--quote-sock` flag > env > default |
| `KAIROS_SIM_ORDER_SOCK` | core (kairos-sim) | Isolated sim order UDS | `<runtime>/kairos-sim-orders.sock` | `--order-sock` flag > env > default |
| `KAIROS_SIM_HUBD` | core (kairos-sim) | Path to the SDK-free `kairos_sim_hubd` binary | auto-discovered under an ancestor of CWD | `--hubd` flag > env > ancestor search |
| `KAIROS_SIM_HUBD_ARGS` | exec (`kairos_sim_hubd`) | Whitespace-split extra args prepended to argv (fault-injection config for drills) | (none) | env, prepended to argv |

`<runtime>` is `$XDG_RUNTIME_DIR` if set, else `/run/user/<uid>`.

### 1.1 Test / drill knobs (not read by production binaries)

These live only in test code (`*/tests/`) and the C++ test CMake, so the census
does not require them, but the drills below need them.

| Variable | Used by | Purpose |
| --- | --- | --- |
| `KAIROS_SIM_BIN` | tui `sim_quote_smoke`, `supervisor_e2e` | Locate the `kairos-sim` binary (default `core/target/release/kairos-sim`) |
| `KAIROS_SCENARIO_SUPERVISORD` | tui `supervisor_e2e` | Locate `kairos_scenario_supervisord` (default `exec/scenario/build/...`) |
| `KAIROS_SUPERVISOR_E2E` | exec `test_scenario_supervisor_e2e` | Gate: set `=1` to run (else self-skips) |
| `KAIROS_FAULT_DRILLS` | exec `test_sim_fault_drills` | Gate: set `=1` to run (else self-skips) |
| `KAIROS_GOLDEN_PATH` | sidecar `test_golden` | Compile-time `-D` macro: path to `quote_golden_envelope.bin` |
| `KAIROS_QUOTE_V2_GOLDEN_PATH` | sidecar `test_golden_v2` | Compile-time `-D` macro: path to `quote_v2_golden_envelope.bin` |
| `KAIROS_TRADE_GOLDEN_PATH` | sidecar `test_trade` | Compile-time `-D` macro: path to `trade_golden_envelope.bin` |

## 2. Sockets & runtime files

All resolve **env > `$XDG_RUNTIME_DIR` > `/run/user/<uid>`**, never `/tmp`. The
C++ (`exec/scenario/src/quote/socket_path.h`) and Rust (`core/src/uds/path.rs`,
`tui/src/sources/*`) resolvers agree byte-for-byte, so core, sidecar, hub and
scenarios all land on the same paths.

| File | Env override | Default name | Writer | Reader(s) |
| --- | --- | --- | --- | --- |
| Quote UDS | `KAIROS_QUOTE_SOCK` | `kairos-quotes.sock` | core | trader, tui, `kairos-uds-client` |
| Order UDS | `KAIROS_ORDER_SOCK` | `kairos-orders.sock` | hub (listens) | scenario traders |
| Supervisor ctl UDS | `KAIROS_SCENARIO_CTL_SOCK` | `kairos-scenario-ctl.sock` | supervisor (listens) | tui / operator |
| Signal UDS | `KAIROS_SIGNAL_SOCK` | `kairos-signals.sock` | signald (listens; later PR) | armed traders (signal client) |
| Hub status JSON | `KAIROS_HUB_STATUS` | `kairos-hub-status.json` | hub | tui |
| Hub halt sentinel | `KAIROS_HUB_HALT` | `kairos-hub-halt` | tui / operator | hub |

Sim namespace (isolated, never collides with live): `KAIROS_SIM_AERON_DIR`
(`/dev/shm/aeron-<user>-sim`), `KAIROS_SIM_QUOTE_SOCK`
(`kairos-sim-quotes.sock`), `KAIROS_SIM_ORDER_SOCK` (`kairos-sim-orders.sock`).
A hard isolation guard refuses to start if any resolved sim path canonicalizes
onto the live Aeron dir or either live socket.

## 3. Config files

Three tomls. Committed templates are `*.example.*`; the live copies are
gitignored (they carry credentials). Env vars override toml only where a row says
so; sockets are auto-resolved and normally need no config.

| File | Template | Lives in | Holds |
| --- | --- | --- | --- |
| Scenario | `exec/scenario/scenario.example.toml` | `exec/scenario/*.toml` (gitignored) | `[scenario]`, `[fees]`, `[pricing]`, `[window]`, `[journal]`, `[risk]`, `[mode]`, `[notify]`, `[dashboard]`. No creds. Supports `base = "..."` inheritance |
| Order hub | `exec/scenario/hub.example.toml` | `exec/scenario/hub.toml` (gitignored) | `[user]` broker creds/PFX; `[hub]` socket_path/journal_dir/order_flow_journal; `[risk]` account-wide gate (notional caps, self-match, fat-finger collars). One hub per broker account |
| Sidecar | `sidecar/concords/sidecar.toml.example` | `sidecar/concords/sidecar.toml` (gitignored) | `[user]` creds/PFX; `[feed]` symbols + `stale_restart_s`; `[reconnect] daily_at`; `[aeron] stream_id` + `dir` |

Precedence recap: journal dir = `[journal].dir` / `[hub].journal_dir` >
`KAIROS_JOURNAL_DIR` > legacy env > `$HOME/Kairos/data/journal`. Order socket =
`[hub].socket_path` > `KAIROS_ORDER_SOCK` > XDG > run-user. Blacklist =
`config_path` > `KAIROS_BLACKLIST_CSV` > lab default. The sidecar Aeron dir comes
from `[aeron] dir` (not `KAIROS_AERON_DIR`).

## 4. Kill switch (admin halt)

Halting stops **all new order submits** at the hub immediately; **cancels keep
flowing** so an operator can flatten open positions. The halt is a sentinel file
(`KAIROS_HUB_HALT`, else `<runtime>/kairos-hub-halt`) whose mere existence the hub
watches. State is mirrored in the hub status JSON (`"halted"`).

From the TUI (Risk tab):
- `k` -> prompts, type the literal word `HALT` + Enter to arm. Anything else, or
  Esc, cancels. This creates the sentinel (`O_CREAT|O_EXCL`; an existing file is
  treated as success).
- `c` -> prompts, type `RESUME` + Enter to clear. Removes the sentinel; an
  already-absent file is success.

By hand (equivalent):
```sh
touch  "$KAIROS_HUB_HALT"   # or "$XDG_RUNTIME_DIR/kairos-hub-halt"  -> arm
rm -f  "$KAIROS_HUB_HALT"   #                                        -> resume
```

## 5. Drill catalog

Every env-gated / `#[ignore]` end-to-end drill and its exact invocation. Rust
drills are `#[ignore]` (excluded from default `cargo test`); C++ drills self-skip
unless their gate env is set. Run from the repo root unless noted. `make drills`
(section 6) runs the full set with all env wired in one place.

Prerequisites (build once):
```sh
cd core && cargo build --release --bins            # kairos-sim, driver, core, replayd, ...
cmake -S exec/scenario -B exec/scenario/build -DCMAKE_BUILD_TYPE=Release
cmake --build exec/scenario/build -j               # sim_hubd, scenario_trader, supervisord, test bins
```

| Drill | Invocation | Needs | Proves |
| --- | --- | --- | --- |
| core `sim_e2e` | `KAIROS_SIM_HUBD=<sim_hubd> KAIROS_SCENARIO_TRADER=<trader> cargo test --test sim_e2e -- --ignored --nocapture` (cwd `core/`) | sim_hubd, trader | Real trader `--live` fills the trend_day tape through the isolated sim, journal fsyncs the fills, clean SIGTERM, no orphan process groups |
| tui `sim_quote_smoke` | `KAIROS_SIM_BIN=<sim> KAIROS_SIM_HUBD=<sim_hubd> cargo test -p kairos-tui --test sim_quote_smoke -- --ignored --nocapture` (cwd `tui/`) | kairos-sim, sim_hubd | The tui feed data source receives real sim quotes |
| core `core_watchdog_e2e` | `cargo test --test core_watchdog_e2e -- --ignored --nocapture` (cwd `core/`) | driver, core, replayd (built) | (A) SIGTERM core mid-stream delivers only whole frames then EOF, exits 0; (B) SIGKILL driver makes core exit nonzero in a bounded window |
| core `tapegen_sim_e2e` | `cargo test --release --test tapegen_sim_e2e -- --ignored --nocapture` (cwd `core/`) | embedded Aeron driver | A generated trend-day tape flows BOTH Quote and Trade events with in-window timestamps |
| core `sim_orphan_startup` | `cargo test --test sim_orphan_startup -- --ignored --nocapture` (cwd `core/`) | kairos-sim built | A SIGTERM during startup still tears every child down (no orphaned sim daemons) |
| core `sim_roundtrip` | `KAIROS_SIM_HUBD=<sim_hubd> cargo test --test sim_roundtrip -- --ignored --nocapture` (cwd `core/`) | sim_hubd | Replay a tiny tape through the isolated sim; a UDS client on the sim quote socket gets quotes; SIGTERM leaves no orphans |
| core `aeron_roundtrip` | `cargo test --test aeron_roundtrip -- --ignored --nocapture` (cwd `core/`) | embedded Aeron driver | A Quote survives an Aeron pub/sub round-trip |
| core `record_roundtrip` | `cargo test --test record_roundtrip -- --ignored --nocapture` (cwd `core/`) | embedded Aeron driver | Recorder captures fragments byte-identical |
| core `replay_roundtrip` | `cargo test --test replay_roundtrip -- --ignored --nocapture` (cwd `core/`) | embedded Aeron driver | Replayed bytes match recorded |
| tui `supervisor_e2e` | `KAIROS_SIM_BIN=<sim> KAIROS_SIM_HUBD=<sim_hubd> KAIROS_SCENARIO_TRADER=<trader> KAIROS_SCENARIO_SUPERVISORD=<supervisord> cargo test -p kairos-tui --test supervisor_e2e -- --ignored --nocapture` (cwd `tui/`) | sim, sim_hubd, trader, supervisord | The Rust supervisor client speaks the S1 wire format; list/start(test)/stop drives a real daemon to running-with-fills and back to stopped; no orphans |
| exec `scenario_supervisor_e2e` | `KAIROS_SUPERVISOR_E2E=1 ctest --test-dir exec/scenario/build -R scenario_supervisor_e2e --output-on-failure` | built exec bins | The C++ supervisor owns and reaps a filling trader end-to-end |
| exec `sim_fault_drills` | `KAIROS_FAULT_DRILLS=1 exec/scenario/build/test_sim_fault_drills` | sim_hubd, trader | Ack-drop storm halt, reject storm, late-fill, disconnect fault-injection drills |
| exec `scenario_restart_e2e` | `ctest --test-dir exec/scenario/build -R scenario_restart_e2e --output-on-failure` | supervisord | Crash-restart backoff, give-up after max retries, and cancel drills (`KAIROS_RESTART_*` injected by the test) |

## 6. `make drills`

`make drills` (repo root) builds core release, checks the exec C++ bins exist
(reusing the `sim-e2e` guard), then runs the full catalog above in sequence,
failing fast per step with a clear line and printing a PASS summary. Build the
exec bins first (see prerequisites). The weekend systemd timer (section 7) runs
only a subset of this.

## 7. Services & timers (deployment-specific, maintained locally)

The systemd units and ops scripts live in `ops/`, which is **gitignored by
design** (env-specific, never pushed). Names and roles for reference:

Services (under `kairos.target`):

| Unit | Runs |
| --- | --- |
| `kairos-driver.service` | `kairos-driver` (Aeron media driver) |
| `kairos-sidecar.service` | `kairos_sidecar sidecar.toml` (broker SDK -> Aeron feed) |
| `kairos-core.service` | `kairos-core` (Aeron -> book -> quote UDS) |
| `kairos-orderhub.service` | `kairos_order_hubd exec/scenario/hub.toml` |
| `kairos-scenario-supervisor.service` | `kairos_scenario_supervisord --scenario-dir exec/scenario` |
| `kairos-recordd.service` | `kairos-recordd` (KQR recorder) |

Timers:

| Timer | Schedule | Runs |
| --- | --- | --- |
| `kairos-sim-drill.timer` | `Sat,Sun 20:00` | `kairos-sim-drill.sh` — a SUBSET of `make drills` (sim-e2e, fault-drills, supervisor-e2e, restart-e2e) |
| `kairos-sentinel.timer` | daily `07:30` | `kairos-sentinel.sh` (pre-open health/creds sentinel) |
| `kairos-watchdog.timer` | every 5 min (`*:0/5`) | `kairos-watchdog.sh` (liveness watchdog) |
| `kairos-record-ship.timer` | (record ship) | `kairos-record-ship.sh` |
| `kairos-bsr.timer` | (lab cron) | lab BSR job |
| `kairos-lab-blacklist.timer` | (lab cron) | blacklist refresh |
| `kairos-lab-chips.timer` / `-ssf` / `-ticks` / `-funding` | (lab crons) | lab data jobs |

Exact schedules, paths and credentials are maintained in the local `ops/` tree,
not here.
