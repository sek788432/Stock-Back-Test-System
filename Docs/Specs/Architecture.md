# Architecture

This spec defines module seams and ownership. The living decisions record the
rationale for the [C++/Qt desktop boundary](../Decisions/ImportantDecisions.md#c-and-qt-desktop-boundary)
and the [project-owned engine](../Decisions/ImportantDecisions.md#engine-and-release-data-authority).
The [canonical result storage decision](../Decisions/ImportantDecisions.md#canonical-result-storage-and-lifecycle)
fixes the Results module, `.bteresult`, lifecycle, and canonical-framing
constraints.

## 1. Status

- **Implemented:** Core, CSV Data, Indicators, Selectable Strategy, basic
  Replay, a limited Engine, Bindings, Frontend, and App targets exist. The Qt
  shell includes its own Backtest page and is optional at configure time.
- **Planned:** the complete module graph below, general Strategy/`MarketSlice`
  seam, Python worker, snapshot builder, complete broker/accounting/metrics,
  canonical results, and `.bteresult` persistence.
- **Blocked for public release:** redistribution rights and a verified redistribution-cleared split manifest.

Planned modules are contracts, not claims about current code.

## 2. Target module graph

Build dependencies are acyclic:

```text
App ──► Frontend ──► Bindings ──► Engine ──► Strategy ──► Indicators ──► Core
                                   │            │                         ▲
                                   ├──────────► Data ────────────────────┤
                                   ├──────────► Metrics ─────────────────┤
                                   └──────────► Results ─────────────────┘

PythonStrategyRunner ──► Strategy + Core
SnapshotBuilder ──► Data + Core
```

- **Core** owns shared value types, time, checked arithmetic, and `Result<T>`.
- **Data** exposes immutable market slices and history from a Release Snapshot.
- **Indicators** owns chronology-safe streaming calculations.
- **Strategy** exposes one narrow command-producing interface for Selectable
  Conditions and Python worker commands.
- **Engine** owns event order, execution, portfolio, risk, and run lifecycle.
- **Metrics** derives values from canonical engine records.
- **Results** owns transactional `.bteresult` persistence and snapshot references.
- **Bindings** translates backend values to queued Qt-facing values; Frontend never reaches into engine internals.

An updater, launcher, repair service, native plugin host, AI action router, and
advanced multi-run scheduler are not V1 modules.

## 3. Engine and strategy seam

The C++ engine is canonical. A Strategy observes a `MarketSlice` and returns commands; it cannot apply fills, mutate accounting, or replace market data.

- Selectable Conditions use a typed C++ plan.
- Python Script Strategies run in a fresh worker process.
- Anonymous pipes carry lifecycle, commands, logs, and errors.
- Application-owned read-only shared memory carries bulk slices and history.
- No TCP, HTTP, localhost port, or Unix-domain socket is used.
- Commands are buffered and committed only after a callback returns successfully.
- A Python exception fails the run transactionally. Cancellation is cooperative for two seconds, then terminates the worker process tree.
- Isolation protects application stability; it is not a security sandbox. Worker inheritance excludes credentials and physical snapshot paths.

## 4. Ownership and threading

| Owner | Mutable state | Communication |
|---|---|---|
| Qt UI thread | Widgets and view models | Queued Qt values only |
| Engine worker | Run state, orders, positions, accounting | Immutable snapshots/events |
| Data worker(s) | Prefetch buffers and snapshot readers | Owned buffers with cancellation |
| Python worker | User strategy object and Python runtime | Versioned IPC only |

Rules:

- No widget access from a worker thread.
- Cross-thread values are immutable or uniquely owned.
- Every worker is RAII-owned and cooperatively cancellable; detached threads are forbidden.
- The Python worker cannot be the authoritative owner of a fill, position, balance, or result.
- V1 permits one active Backtest. Advanced multi-run concurrency is outside the
  accepted scope rather than a latent extension contract.

## 5. Data and result seam

- Release execution reads an immutable snapshot produced from tracked `StockData/Extracted`.
- DuckDB is a developer ingestion/verification store owned by `DataFetcher/`; it is not a user-selectable release data source.
- Results reference content-addressed Data Segments. When Python ran, they
  retain Runtime Profile identity and hash metadata for provenance, but replay
  does not require that profile to be installed and does not pin it.
- One run produces one versioned SQLite `.bteresult` file through staging, transactions, validation, and atomic promotion.
- K-line Replay combines persisted canonical events with retained snapshot segments; it does not duplicate OHLCV into each result.

## 6. Error seam

Every fallible public C++ backend function is `[[nodiscard]]` and returns `bte::core::Result<T>`. `Error` is the fixed error payload of that class; do not spell the type `Result<T, Error>`. Internal exceptions must be caught and translated before crossing a module seam.

## 7. Compatibility identities

Every completed result identifies at least:

- engine and result-schema versions;
- strategy source/artifact hash and strategy API version;
- Run Configuration and numeric-policy versions;
- Release Snapshot, Data Segment, and calendar hashes;
- Python runtime profile when Python executed;
- `canonicalResultHash` over canonical functional records.

Wall-clock creation time, local paths, and SQLite page layout are not functional determinism inputs.
