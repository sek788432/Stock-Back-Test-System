# 04 — Data Layer

This spec owns market-data provenance, release snapshots, chronology-safe access, and result-reference retention. It separates developer ingestion from release execution.

## 1. Status

- **Implemented:** `DataFetcher/` writes DuckDB and extracts CSV; C++ `CsvBarStream` reads tracked `StockData/Extracted/<SYMBOL>.csv` files into memory.
- **Planned:** release snapshot builder, immutable segment reader, streaming/prefetch, synchronized multi-symbol slices, calendar/split manifests, retention, and research access.
- **Blocked for public release:** redistribution rights for derived market data and a verified redistribution-cleared split manifest are not recorded.

The existing CSV reader is a development baseline. It is not yet the release snapshot implementation and currently materializes a complete file.

## 2. Source roles

| Source | Role | Release runtime authority |
|---|---|---|
| Databento | Upstream developer acquisition source | No |
| `DataFetcher/` | Developer-only ingestion, validation, and extraction pipeline | No |
| `StockData/MarketData.duckdb` | Mutable developer store owned by Python | No |
| `StockData/Extracted` | Tracked input to the release snapshot build | Build input only |
| Release Snapshot | Immutable, validated, versioned application dataset | Yes |

C++ must never write the DuckDB file. V1 users cannot select an arbitrary database, import pricing data, or provide provider credentials. Database integration is deferred.

## 3. Release snapshot build

Release CI, not the application runtime, builds the snapshot:

1. Read only tracked extract files from the release revision.
2. Validate schema, finite values, OHLCV invariants, symbol identity, timestamps, ordering, and duplicate policy.
3. Normalize prices to `Price` nanodollars and volume to `Quantity` microshares with checked conversion.
4. Attach the versioned US-equities calendar and verified split events.
5. Partition bars into immutable content-addressed Data Segments.
6. Write a manifest containing schema version, source revision, timeframe/profile, ranges, counts, segment hashes, calendar hash, split-manifest hash, and snapshot ID.
7. Re-read and verify every artifact before packaging.

The snapshot profile is `vendorExtendedHours`: every actual hourly row is eligible, including pre-market and after-hours rows. The product must not label this regular-session-only data.

## 4. Release reader contract

The planned reader accepts a snapshot ID, universe, timeframe, half-open date range, and history budget. It returns `Result<T>` for discovery and access.

- Bars are emitted in stable timestamp then symbol order.
- One `MarketSlice` contains the actual bars available at one timestamp.
- Missing bars remain absent; strategy and indicator history is never forward-filled.
- A portfolio may use its last actual mark only when explicitly labeled stale.
- History cannot pass the current strategy timestamp.
- Executable history is capped at 10,000 bars per symbol and 5,000,000 retained bars across the universe; excess requests fail, not truncate.
- Streaming and bounded prefetch are required; release execution must not materialize the entire universe.

`stockbt.strategy` receives chronology-safe current/past views without physical snapshot paths. Separate `stockbt.research` access may inspect the full immutable snapshot, but research output is not a verified Backtest Result.

## 5. Calendar and corporate actions

The snapshot carries immutable exchange dates, `America/New_York` session identity, regular/extended boundaries, holidays, early closes, UTC conversion rules, and a content hash.

- DAY expiry uses this calendar.
- A verified split is applied before the first executable slice on its effective exchange date, or before the symbol's next actual bar if none exists that date.
- Positions, average cost, active orders, reservations, and relevant indicator history adjust together.
- Split remainder handling follows the fixed-point policy and emits a persisted adjustment.
- Price gaps must never be used to infer a split.
- Dividends are not modeled in V1. Results record `dividendAccounting: excluded` and disclose price-return limitations for both long and short strategies.

## 6. Result references and retention

A `.bteresult` stores snapshot/segment IDs and hashes, not duplicate OHLCV.

- Referenced Data Segments are retained while any non-purged result needs them.
- K-line Replay resolves those exact segments and validates their hashes.
- Missing referenced data yields `DataSnapshotUnavailable`; it never silently substitutes newer data.
- Imported results validate result schema, canonical hash, snapshot identity, segment hashes, and runtime references before replay.
- Result deletion uses recoverable Trash first; permanent purge releases segments only when reference counts reach zero.

## 7. Legal release gates

Before any public artifact includes market data, release evidence must record:

- the right to redistribute every included derived dataset;
- the source and redistribution terms for the split manifest;
- the exact snapshot hashes covered by that evidence.

Until then, snapshot tooling may be developed and tested with repository fixtures, but a public data-bearing release is **Blocked**.

## 8. Verification requirements

- Unit tests: positive, negative, and boundary behavior for parsing, normalization, range selection, gaps, duplicate timestamps, seek/history limits, calendar edges, and split adjustment.
- Contract/integration tests: extract-to-snapshot conversion, manifest/hash validation, deterministic segment generation, bounded streaming, `.bteresult` reference resolution, and missing/corrupt segment errors.
- Performance tests: configured universe streams within the documented memory and runtime budgets.
- A check is merge-blocking only after its implementation exists and the workflow requires it.
