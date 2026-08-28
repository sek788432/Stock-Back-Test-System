# Data Layer

This spec owns market-data provenance, release snapshots, chronology-safe access, and result-reference retention. It separates developer ingestion from release execution.

## 1. Status

- **Implemented:** `DataFetcher/` writes DuckDB and extracts per-symbol CSV;
  C++ `CsvBarStream` reads tracked files under `StockData/Extracted` into
  memory. The direct reader currently supports symbols whose CSV filename stem
  exactly equals the canonical symbol; dotted-symbol exports are a known gap.
- **Planned:** release snapshot builder, immutable segment reader,
  streaming/prefetch, synchronized multi-symbol slices, calendar/split
  manifests, and retention.
- **Blocked for public release:** redistribution rights for derived market data and a verified redistribution-cleared split manifest are not recorded.

The existing CSV reader is a development baseline. It is not yet the release snapshot implementation and currently materializes a complete file.

## 1.1 V1 data flow

V1 separates **developer data preparation** from **application data access**.
The handoff between them is the reviewed CSV set under
`StockData/Extracted/`; DuckDB is not a V1 runtime dependency.

```text
Databento Historical API
        |
        |  DataFetcher/FetchDatabento.py
        v
StockData/MarketData.duckdb
  mutable developer ingestion and verification store
        |
        |  DataFetcher/GetFromDb.py
        v
StockData/Extracted/<FILE-STEM>.csv
  reviewed V1 data input
        |
        +--> current implementation: CsvBarStream reads CSV directly
        |
        `--> accepted release V1: release CI builds an immutable Release Snapshot
```

The boundary has these consequences:

1. `FetchDatabento.py` acquires Databento bars and validates/upserts them into
   `StockData/MarketData.duckdb` for developer use.
2. `GetFromDb.py` exports the selected DuckDB rows to one CSV per symbol under
   `StockData/Extracted/`. Its current filename mapping replaces `.` with `_`,
   so `BRK.B` is written to `BRK_B.csv`; the row retains canonical symbol
   `BRK.B`.
3. The current C++ implementation reads CSV through `CsvBarStream`, which opens
   `<requested-symbol>.csv` and requires every row's symbol to equal that
   request. It does not open DuckDB or contact Databento. Because it does not
   yet reverse or otherwise resolve the export filename mapping, dotted symbols
   such as `BF.B` and `BRK.B` are not loadable through this direct reader.
4. The accepted release V1 also starts from the reviewed extracted CSV files,
   but release CI converts them into the immutable, content-addressed Release
   Snapshot described below. The packaged application reads that snapshot, not
   the mutable developer DuckDB file.
5. Updating DuckDB alone cannot change application or release inputs. A change
   becomes a candidate V1 input only after re-extraction, validation, review,
   and inclusion of the resulting CSV revision.

The planned Release Snapshot uses the canonical symbol stored in each validated
row/manifest entry; an extract filename is transport metadata and cannot define
snapshot identity. Snapshot building must reject ambiguous filename mappings
or symbol mismatches rather than silently merging them.

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
5. Partition bars into immutable content-addressed Data Segments by symbol,
   timeframe, and UTC calendar year. Hash canonical segment bytes; file paths
   and container layout are not identity.
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

Project-owned `stockbt` receives chronology-safe current/past views without
physical snapshot paths. There is no full-snapshot research API.

### 4.1 Canonical Data Segment bytes

Data Segment schema `1.0` is one byte stream in this exact order. Every integer
uses unsigned or two's-complement big-endian encoding as stated; strings are
their exact validated UTF-8 bytes without a terminator.

| Field | Encoding |
|---|---|
| Domain separator | ASCII bytes `BTE-DATA-SEGMENT`, followed by one zero byte |
| Schema | unsigned 16-bit major, then unsigned 16-bit minor |
| Symbol | unsigned 16-bit byte length, then UTF-8 bytes |
| Timeframe | unsigned 16-bit byte length, then UTF-8 bytes |
| UTC year | signed 32-bit integer |
| Row count | unsigned 64-bit integer |
| Rows | exactly `row count` records using the layout below |

Each row is exactly six signed 64-bit integers: UTC Unix epoch milliseconds,
open nanodollars, high nanodollars, low nanodollars, close nanodollars, volume
microshares, in that order. Rows are strictly ascending by timestamp. A row
outside the header's UTC year, a duplicate/nonascending timestamp, invalid
UTF-8, an invalid OHLCV value, trailing bytes, or a count/length mismatch makes
the segment invalid.

The segment ID is lowercase hexadecimal SHA-256 over the complete byte stream,
including the domain separator and header. Paths, compression, archive
containers, filesystem metadata, and manifest ordering are excluded. A change
to any byte-level rule requires a new schema major; readers never reinterpret
old bytes under a newer schema.

## 5. Calendar and corporate actions

The snapshot carries one canonical calendar byte stream. Calendar schema `1.0`
starts with ASCII `BTE-TRADING-CALENDAR` plus one zero byte, unsigned 16-bit
big-endian major and minor versions, an unsigned 16-bit length plus the exact
UTF-8 timezone ID (`America/New_York` for V1), then an unsigned 64-bit row count.

There is exactly one fixed-width row for every Gregorian civil date in the
manifest's inclusive covered range, strictly ascending with no gap or
duplicate. Each row contains, in order:

- signed 32-bit `YYYYMMDD`;
- unsigned 8-bit trading-day flag (`0` closed, `1` trading day; other values
  invalid);
- signed 64-bit extended-session open UTC epoch milliseconds;
- signed 64-bit regular-session open UTC epoch milliseconds;
- signed 64-bit regular-session close UTC epoch milliseconds;
- signed 64-bit extended-session close UTC epoch milliseconds.

All integers are big-endian. A closed date, including weekends and holidays,
uses zero for all four timestamps. A trading date requires strictly increasing
nonzero boundaries; an early close is represented by its actual regular and
extended close timestamps. Invalid dates, flags, order, boundaries, count, or
trailing bytes reject the calendar. `calendarHash` is lowercase hexadecimal
SHA-256 over the complete stream. Runtime timezone-database interpretation,
holiday names, paths, and container layout are excluded from identity. A
byte-level change requires a new schema major. The manifest field containing
this digest is `calendarHash`; it is external to the hashed stream.

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
- Imported results validate result schema, canonical hash, snapshot identity,
  segment hashes, and Runtime Profile metadata syntax/hash fields before replay.
  Runtime Profile availability is not required because replay never executes
  embedded source.
- Result deletion uses recoverable Trash first. Active and trashed results pin
  their segments. After 30 days, purging a result releases those references;
  an unreferenced Data Segment moves to Data Trash and becomes permanently
  removable after its own 30-day recovery period.

## 7. Legal release gates

Before any public artifact includes market data, release evidence must record:

- the right to redistribute every included derived dataset;
- the source and redistribution terms for the split manifest;
- the exact snapshot hashes covered by that evidence.

Until then, snapshot tooling may be developed and tested with repository fixtures, but a public data-bearing release is **Blocked**.

## 8. Verification requirements

- Unit tests: positive, negative, and boundary behavior for parsing, normalization, range selection, gaps, duplicate timestamps, seek/history limits, calendar edges, and split adjustment.
- Contract/integration tests: extract-to-snapshot conversion, manifest/hash validation, deterministic segment generation, bounded streaming, `.bteresult` reference resolution, and missing/corrupt segment errors.
- The performance-test implementation must provide a checked-in deterministic
  synthetic-fixture generator for 100
  symbols by 10,000 hourly bars (1,000,000 bars total). Symbols are `BTE000`
  through `BTE099`; all symbols have one bar at each consecutive UTC hour from
  `2020-01-01T00:00:00Z`. For symbol index `s` and timestamp index `t`, in
  authoritative integer units:
  - `open = 100000000000 + s * 1000000000 + t * 10000` nanodollars;
  - `close = open + (((t + s) % 3) - 1) * 1000` nanodollars;
  - `high = max(open, close) + 2000` nanodollars;
  - `low = min(open, close) - 2000` nanodollars;
  - `volume = 1000000000 + s * 1000000 + (t % 100) * 1000` microshares.
- That implementation must check in the generator, manifest, and expected
  SHA-256 identities as fixture authority; generated data remains build output
  rather than tracked repository data.
- The implementation must add a checked-in `benchmark` configure/build preset
  that inherits `release`, enables `BTE_BUILD_TESTS=ON` and
  `BTE_BUILD_BENCHMARKS=ON`, and builds the registered
  `bte_data_benchmark` target. Until that preset, option, target, and fixture
  exist, this is **Planned / not merge-blocking**.
- Measure five fresh-process `bte_data_benchmark` runs on the
  standard GitHub-hosted `ubuntu-24.04` x64 runner. Record runner image release,
  CPU model/core count, RAM, application revision, preset, and all raw samples.
  The median must produce the first usable slice within two seconds, sustain at
  least 100,000 bars/second from that slice through final consumption, and keep
  peak resident memory no more than 768 MiB above the post-initialization,
  pre-open process baseline.
- A check is merge-blocking only after its implementation exists and the workflow requires it.
