# Kairos cross-broker feed normalization

Normative spec for how every market-data sidecar maps its broker feed onto the
shared `kairos.capnp` wire format. A new sidecar (e.g. the D2 fubon feed) can be
implemented against this document without reading any other sidecar's code.

The wire schema is **append-only**: field ordinals are never renumbered, retyped,
or removed. Cap'n Proto does *not* trim trailing zero words on serialize, so a
Quote encoded under the current schema is larger than one encoded before the A2
fields were added; older recordings still decode, with every absent field taking
its capnp default (0 / first enumerant). Decoders MUST tolerate this.

## 1. Envelope variants

The data stream (`aeron:ipc` stream 1001, recorded verbatim as KQR) carries
`Envelope` messages. Two variants are market data:

- `quote @0 :Quote` — an order-book **DEPTH** snapshot (top-N bids/asks) plus a
  best-effort last-trade snapshot.
- `trade @6 :Trade` — a single, authoritative **TRADE** print.

Other variants (`subscribe`, `unsubscribe`, `subAck`, `error`, `heartbeat`) are
control frames and never appear as recorded market data. A decoder that only
wants quotes MUST treat any non-`quote` variant as "not a quote" without erroring;
core counts unhandled variants (`unknown_variants`) rather than dropping silently.

### DEPTH vs TRADE — why they are separate events

DEPTH and TRADE are distinct events on the wire. Offline fill simulation and the
hftbacktest converter (A4/A5) need both, and overloading `Quote.lastPrice == 0`
to mean "no trade this tick" would pollute the queue model. fubon and shioaji
already deliver book and trade as **separate** callbacks, so they map one broker
event to one wire event. concords delivers a **combined** `Quotation` object
carrying both depth and (optionally) a trade; its sidecar splits each callback
into one `Quote` and, when the object carries a trade, one additional `Trade`.

## 2. Source registry (`source :UInt16`)

Every Quote and Trade carries the feed origin so a dual-feed recording is
self-describing and the compare tool can group by source.

| value | broker           | status   |
|-------|------------------|----------|
| 0     | concords (康和)  | in use   |
| 1     | fubon (富邦 neo) | reserved |
| 2     | shioaji (永豐)   | reserved |

A new source claims the next free value here in the same PR that adds its sidecar.

## 3. seq / epoch (gap detection)

- `epoch :UInt32` — the **feed-session generation**. 0 is the legacy/no-epoch
  sentinel. Each ticker/session build assigns a new epoch that is strictly
  greater than any the process has used **and** than any a prior process could
  have used: it is a process-global counter **seeded from wall-clock seconds**
  and advanced on **every** ticker/session rebuild — the initial build, the daily
  reconnect, the staleness-watchdog rebuild, and any other reconnect. Seeding
  from the clock matters because `recordd` appends to one dated KQR file across a
  sidecar restart, so a restarted process must not reuse the previous process's
  epoch space. All symbols in a session share the current epoch.
- `seq :UInt64` — a **per-`(source, symbol)`** monotonic counter, +1 for **every**
  emitted wire event for that symbol. Quote and Trade for one symbol share a
  single seq space, so a full-stream consumer (A4/A5) sees a contiguous run and
  detects any loss. Per-symbol seq **resets** on an epoch bump.

Interpretation:

- Gap in seq **within the same epoch** → data loss.
- seq reset **accompanied by an epoch bump** → benign session rebuild, not loss.
- `seq == 0 && epoch == 0` → a legacy record with no seq/epoch; no gap detection.

A consumer that only reads `Quote` (ignoring `Trade`) will see a **non-contiguous**
seq (the Trade seqs are missing). This is intentional: only a full-stream consumer
that reads both variants should run gap detection. Do not false-alarm on it.

A fubon/shioaji sidecar MUST implement the same tracker: assign a fresh epoch
(process-global, wall-clock-seeded, strictly increasing) on every (re)connect and
process (re)start, and assign the next per-symbol `seq` to each emitted
Quote/Trade. Epoch values need not match across sources — the compare tool tracks
seq gaps per source and does not align epochs across feeds.

## 4. Timestamps

- `Quote.quoteTsUs` / `Trade.tradeTsUs` — the **broker's** event timestamp in
  microseconds since the Unix epoch (UTC). If the broker gives coarser
  granularity, scale up to microseconds; if it gives none, set 0.
- `recvTsUs` — the **sidecar's** receive time, `CLOCK_REALTIME` microseconds,
  stamped the moment the callback fires (before the Aeron offer). Used for
  cross-source latency/skew comparison. 0 means legacy/not stamped.
- The KQR record header additionally stamps a recorder-side `recv_ts_us`; that is
  the *recorder's* clock, independent of the sidecar's `recvTsUs`.

## 5. Prices (`priceMantissa` + `priceScale`)

Prices are integer **mantissa + scale**: value = `mantissa * 10^(-scale)`. A
broker reporting 580.50 may send `(58050, scale 2)` or `(5805, scale 1)`; both are
valid and equal. Consumers MUST compare scale-aware (cross-multiply, no float);
the compare tool does exactly this. Each sidecar sets `priceScale` to whatever its
broker's price precision is — do not force a common scale.

`exec` converts mantissa+scale to integer cents via `mantissa * 10^(2 - scale)`
(truncating for scale > 2); that is an exec-side convenience, not a wire rule.

## 6. Volumes

`volume` (per depth level, and `Trade.volume`) is in the **broker's native lot
unit**. For TWSE equities that is shares for odd-lot feeds and lots (1 lot = 1000
shares) for round-lot feeds — see `board`. A sidecar MUST document its unit here
when it lands; concords reports the raw SDK volume unchanged. Do not silently
convert lots↔shares on the wire; downstream converts using `board` + symbol.

## 7. board (`QuoteBoard`)

`QuoteBoard { unknown @0; roundLot @1; oddLot @2; }` marks the lot board a Quote
belongs to. It is a **new quote-side enum**, deliberately *not* the order-wire
`Board` (whose `@0` is `oddLot`): a newly-added enum field decodes to `@0` on old
records, and `QuoteBoard.unknown @0` keeps historical round-lot equity quotes from
being mislabelled odd-lot. `unknown` also covers feeds where the board is not yet
distinguished. The concords equity feed sets `roundLot`.

## 8. session (`Session`) — futures/night market

`Session { unknown @0; day @1; night @2; }`. Equity/legacy records leave it
`unknown`. The futures work (E1/E2) sets `day`/`night` per the TAIFEX session.

## 9. Quote.last* — deprecated last-trade snapshot

`Quote.lastPrice @5 / lastScale @6 / lastVolume @7` are a **best-effort** last-trade
snapshot retained for back-compat: old recordings carry them and current exec
`UdsQuoteClient` reads `lastPrice`/`lastVolume`. They are **deprecated**; the
authoritative trade is the `Trade` event. During the transition the concords
sidecar keeps populating them (from the combined `Quotation`'s trade) so exec needs
no change. A post-A5 cleanup may stop populating them once no consumer reads them;
the ordinals are **never** removed or renumbered. A new sidecar SHOULD populate
`last*` from its most recent trade if cheap, but consumers must not rely on them
for fill logic.

## 10. E2 futures fields (reserved, populated later)

Added now on both Quote and Trade so E2 is a pure "enable", not a schema change:

- `session` — see §8.
- `tradingDate :UInt32` — the exchange trading date as `yyyymmdd` (a night session
  belongs to the *next* trading date; that mapping is the sidecar's job).
- `simtrade :Bool` — the broker's simulated-session flag (e.g. shioaji `simtrade`).
  Distinct from `Quote.isTrial` / `Trade.isTrial`, which is the equity 試撮
  (pre-open trial-match) flag. A record may be `isTrial` (trial match) without
  being `simtrade` (simulated feed), and vice versa.
- `underlyingPrice :Int64` — underlying price mantissa for a future/option
  (`priceScale` shared with the instrument). 0 until E1/E2.

Equity sidecars leave all four at their defaults.

## 11. Field reference

### Quote

| field           | ord | type       | notes                                        |
|-----------------|-----|------------|----------------------------------------------|
| symbol          | 0   | Text       | broker product id                            |
| exchange        | 1   | Exchange   | twse/tpex/tfx/otc                            |
| quoteTsUs       | 2   | Int64      | broker ts, µs (§4)                           |
| bids            | 3   | List(PriceLevel) | descending, top-N                      |
| asks            | 4   | List(PriceLevel) | ascending, top-N                       |
| lastPrice       | 5   | Int64      | deprecated snapshot (§9)                      |
| lastScale       | 6   | UInt8      | deprecated snapshot (§9)                      |
| lastVolume      | 7   | Int64      | deprecated snapshot (§9)                      |
| isTrial         | 8   | Bool       | equity 試撮                                   |
| source          | 9   | UInt16     | §2                                            |
| seq             | 10  | UInt64     | §3                                            |
| epoch           | 11  | UInt32     | §3                                            |
| recvTsUs        | 12  | Int64      | sidecar recv, µs (§4)                         |
| board           | 13  | QuoteBoard | §7                                            |
| session         | 14  | Session    | §10, reserved                                 |
| tradingDate     | 15  | UInt32     | §10, reserved                                 |
| simtrade        | 16  | Bool       | §10, reserved                                 |
| underlyingPrice | 17  | Int64      | §10, reserved                                 |

### Trade

| field           | ord | type     | notes                        |
|-----------------|-----|----------|------------------------------|
| symbol          | 0   | Text     | broker product id            |
| exchange        | 1   | Exchange |                              |
| source          | 2   | UInt16   | §2                           |
| seq             | 3   | UInt64   | §3                           |
| epoch           | 4   | UInt32   | §3                           |
| tradeTsUs       | 5   | Int64    | broker ts, µs (§4)           |
| recvTsUs        | 6   | Int64    | sidecar recv, µs (§4)        |
| priceMantissa   | 7   | Int64    | §5                           |
| priceScale      | 8   | UInt8    | §5                           |
| volume          | 9   | Int64    | §6                           |
| isTrial         | 10  | Bool     | equity 試撮                   |
| session         | 11  | Session  | §10, reserved                |
| tradingDate     | 12  | UInt32   | §10, reserved                |
| simtrade        | 13  | Bool     | §10, reserved                |
| underlyingPrice | 14  | Int64    | §10, reserved                |

## 12. Per-broker mapping

### concords (康和), source 0 — implemented

- One `Quotation` callback → one `Quote` (depth + `last*` from `GetTrade(0)`) and,
  when `GetTradeSize() > 0`, one `Trade`.
- `quoteTsUs` / `tradeTsUs` from `Quotation::GetTimestamp()` (µs).
- `recvTsUs` = `CLOCK_REALTIME` µs at callback entry.
- `priceMantissa`/`priceScale` from the SDK `PriceVolume.price` (`digits` /
  `precision`); `volume` is the raw SDK volume.
- `board` = `roundLot` (equity round-lot feed); `session`/`tradingDate`/`simtrade`/
  `underlyingPrice` left default.
- `isTrial` from `Quotation::IsTrial()`.
- `epoch` assigned in the ticker `build()` (process-global, wall-clock-seeded);
  `seq` per symbol per emitted event.

### fubon (富邦 neo), source 1 — TBD

fubon delivers book and trade as separate callbacks (map each 1:1 to `Quote` /
`Trade`). Fill in volume unit, price precision, and timestamp timebase when the D2
sidecar lands. It is a standby/secondary feed for cross-checking source 0.

### shioaji (永豐), source 2 — TBD

shioaji delivers separate book/trade events and exposes a `simtrade` flag (→ the
`simtrade` field) and TAIFEX sessions (→ `session`/`tradingDate`). Fill in units
and timebase when that sidecar lands.

## 13. Consistency tooling

`kairos-feed-compare` (core `src/bin/feed_compare.rs`) reads two KQR recordings —
or one recording carrying two `source`s — and reports, per shared symbol:
per-source seq gaps (§3), scale-aware trade-price/volume mismatches with their
`tradeTsUs` skew, and final top-of-book mismatches. It exits non-zero on any
divergence. This is the D2 bring-up harness for validating a second feed against
concords; it is offline and file-based only.
