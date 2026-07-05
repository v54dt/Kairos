# Kairos

Taiwan + crypto arbitrage infrastructure: a market-data pipeline feeding an
execution engine, with recording and monitoring around it.

## Components

| Path             | Language | Binary / artifact       | Role                                                        |
| ---------------- | -------- | ----------------------- | ----------------------------------------------------------- |
| `schema/`        | Cap'n Proto | `kairos.capnp`       | Wire schema shared by every component                       |
| `core/`          | Rust     | `kairos-core` + tools   | Aeron → book → UDS quote server; recorder, replay, verify   |
| `core/`          | Rust     | `kairos-sim`            | One-command isolated sim universe (driver+core+sim hub) for offline testing |
| `sidecar/concords/` | C++   | sidecar daemon          | Broker SDK → Aeron feed publisher                           |
| `exec/scenario/` | C++      | scenario engine         | Strategy scenarios + order hub over UDS                     |
| `tui/`           | Rust     | `kairos-top`            | Terminal health dashboard (feeds, books, scenarios, jobs, events) |

## Build

Rust core:

```sh
cd core && cargo build
```

TUI (`kairos-top`):

```sh
cd tui && cargo build --release
./target/release/kairos-top            # --symbols 2330,0050 --data-dir ~/Kairos/data
```

`kairos-top` has four tabs, switched with `1`-`4` or `Tab`: **Overview**
(systemd, feed, recorder, events), **Feeds & Books** (L5 book viewer),
**Scenarios** (order-journal rollups + live order-hub status), and **Data &
Events** (jobs & timers, blacklist freshness, recorder archive, and a wider
journald event tail). It is read-only (its only write is the UDS Subscribe
frame). Quit with `q` or `Ctrl-C`; the terminal is restored on quit and on panic.

C++ components:

```sh
cmake -S sidecar/concords -B sidecar/concords/build -DCMAKE_BUILD_TYPE=Release
cmake --build sidecar/concords/build -j
cmake -S exec/scenario -B exec/scenario/build -DCMAKE_BUILD_TYPE=Release
cmake --build exec/scenario/build -j
```

## Testing anytime with the sim

`kairos-sim` brings up a complete, ISOLATED pipeline — its own Aeron media driver,
`kairos-core`, and the SDK-free `kairos_sim_hubd` order hub — in a namespace that
can never collide with the live `kairos.target`. It uses a `-sim` Aeron dir
(`$KAIROS_SIM_AERON_DIR`, default `/dev/shm/aeron-<user>-sim`) and separate quote
/order sockets (`$KAIROS_SIM_QUOTE_SOCK`, `$KAIROS_SIM_ORDER_SOCK`). A hard
isolation guard refuses to start if any resolved sim path canonicalizes onto the
live Aeron dir or either live socket, so a fat-fingered override can never publish
onto — or trade against — the live pipeline.

```sh
cd core
cargo run --bin kairos-sim -- up --symbols 2330,2317        # live, book-only sim
cargo run --bin kairos-sim -- replay data/kqr/.../s1001-*.kqr --symbols 2330 --speed 60
```

On startup it prints the exact env to export in another shell so
`kairos-uds-client` / `kairos-top` (or a scenario trader) attach to the sim
instead of live:

```sh
export KAIROS_AERON_DIR=/dev/shm/aeron-<user>-sim
export KAIROS_QUOTE_SOCK=/run/user/<uid>/kairos-sim-quotes.sock
export KAIROS_ORDER_SOCK=/run/user/<uid>/kairos-sim-orders.sock
```

Ctrl-C tears down every child (process-group kill, no orphans). `kairos-sim down`
kills a running sim from another shell (idempotent); `kairos-sim status` reports
what is up.
