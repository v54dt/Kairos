# Kairos

Taiwan + crypto arbitrage infrastructure: a market-data pipeline feeding an
execution engine, with recording and monitoring around it.

## Components

| Path             | Language | Binary / artifact       | Role                                                        |
| ---------------- | -------- | ----------------------- | ----------------------------------------------------------- |
| `schema/`        | Cap'n Proto | `kairos.capnp`       | Wire schema shared by every component                       |
| `core/`          | Rust     | `kairos-core` + tools   | Aeron → book → UDS quote server; recorder, replay, verify   |
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
