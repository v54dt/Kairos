# Golden synthetic KQR tapes

Small, byte-deterministic stream-1001 market-data tapes produced by
`kairos-tapegen`, used by the golden regression `core/tests/golden_tapegen.rs`
(regenerate == committed, byte-for-byte) and by the off-hours sim/TUI harness.
All timestamps fall in the continuous TWSE session 09:00-13:25 Taipei on
2026-07-03, symbol `2330`, ~2 minutes at a 1 s tick.

Each file is regenerated in CI from the matching `GenParams::fixture_*` inputs,
which map exactly to the command below.

| File | Command | Expected behavior |
|------|---------|-------------------|
| `quotes_only_2330.kqr` | `kairos-tapegen --scenario quotes-only --symbol 2330 --out quotes_only_2330.kqr` | Pure quote flow, no trades; drives the TUI/data-source, zero fills possible. |
| `trend_day_2330.kqr` | `kairos-tapegen --scenario trend-day --symbol 2330 --out trend_day_2330.kqr` | Steady downward drift; trades walk a descending ladder that prints strictly below the join reference, so a resting join/TWAP BUY fills. |
| `limit_lock_2330.kqr` | `kairos-tapegen --scenario limit-lock --symbol 2330 --out limit_lock_2330.kqr` | Bid pinned at the daily limit with a growing queue; every trade prints AT the limit, none crosses, so a resting BUY at the limit stays unbuyable. |

Defaults baked into `fixture_*`: `--date 20260703 --seconds 120 --tick-ms 1000
--seed 1 --base-price 58000 --scale 2`.

## Follow-on scenarios (documented, not yet generated)

- `thin-book` — wide spread / tiny depth, partial-fill / slippage tests.
- `trial-session` — `isTrial` quotes in the pre-open band (needs
  `--allow-out-of-window`) that must NOT fill.
- `night-session-spike` and `disposal-split-matching` (A4 auction) — larger,
  deferred.

## End-to-end (manual, needs the embedded Aeron driver)

Replay a tape through an isolated sim and watch quotes/trades (and, with a
trader attached, fills) flow:

```
core/target/release/kairos-sim replay core/tests/fixtures/tapes/trend_day_2330.kqr \
  --symbols 2330 --bin-dir core/target/release --hubd exec/scenario/build/kairos_sim_hubd
```

The automated version is the `#[ignore]` test `core/tests/tapegen_sim_e2e.rs`:

```
cargo test --release --test tapegen_sim_e2e -- --ignored
```
