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
  Data also implements limited immutable snapshot/segment support; Results
  implements transactional `.bteresult` storage consumed by result Replay.
  The internal Python backend also exists: bounded protocol, POSIX process
  ownership, bootstrap, runtime-manifest verification, typed Strategy adapter,
  C++ SMA scheduling, and bounded history, covered by registered tests. The
  optional macOS Apple Silicon app build assembles and verifies the pinned
  runtime profile; the default developer build omits that profile.
- **Planned:** the complete module graph below, general `MarketSlice`, the
  complete release snapshot pipeline,
  broker/accounting/metrics, and remaining canonical result families.
- **Blocked for public product release:** redistribution rights and a verified redistribution-cleared split manifest.

Planned modules are contracts, not claims about current code.

## 2. Target module graph

Build dependencies are acyclic:

```text
App ──► Frontend ──► Bindings ──► Engine ──► Strategy ──► Indicators ──► Core
                         │         │            │                         ▲
                         │         ├──────────► Data ────────────────────┤
                         │         ├──────────► Metrics ─────────────────┤
                         │         └──────────► Results ─────────────────┘
                         └────► PythonStrategyRunner ──► Strategy + Core
SnapshotBuilder ──► Data + Core
```

- **Core** owns shared value types, time, checked arithmetic, and `Result<T>`.
- **Data** exposes immutable market slices and history from a Release Snapshot.
- **Indicators** owns chronology-safe streaming calculations.
- **Strategy** exposes one narrow command-producing interface for Selectable
  Conditions and Python worker commands. Its public context, callback, and
  command values contain no Qt or process handles.
- **Engine** owns event order, execution, portfolio, risk, and run lifecycle.
- **PythonStrategyRunner** is constructed and owned by Bindings for one
  execution. It owns the fresh worker, pipes, read-only shared-memory handles,
  startup and hook deadlines, cancellation, and cleanup. It implements the
  Strategy callback seam; Engine has no dependency on this module and never
  creates or supervises an OS process.
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
- The first Python milestone uses one selected symbol and the same Engine path
  as Selectable Conditions. It registers C++ SMA indicators before warm-up,
  provides immutable current/history/portfolio values, and accepts only
  whole-share long market entry and full-position close commands. The Python
  adapter cannot bypass Engine validation, next-actual-open chronology,
  accounting, or Result recording.

## 4. Ownership and threading

| Owner | Mutable state | Communication |
|---|---|---|
| Qt UI thread | Widgets and view models | Queued Qt values only |
| Engine worker | Run state, orders, positions, accounting | Immutable snapshots/events |
| Data worker(s) | Prefetch buffers and snapshot readers | Owned buffers with cancellation |
| Python worker | User strategy object and Python runtime | Versioned IPC only |
| PythonStrategyRunner | One worker process, its pipes, and shared-memory leases | Owned by the active Bindings execution; reports immutable callback values |

Rules:

- No widget access from a worker thread.
- Cross-thread values are immutable or uniquely owned.
- Every worker is RAII-owned and cooperatively cancellable; detached threads are forbidden.
- The Python worker cannot be the authoritative owner of a fill, position, balance, or result.
- Bindings freezes source and parameters before consent and worker creation. A
  later editor change is a new draft and cannot mutate the active execution.
- Pre-writer worker startup, module-load, and factory failures create no Result
  artifact. Once the writer starts, hook failure/cancellation preserves only
  the trusted committed Engine prefix and terminal diagnostics under the
  existing explicit Save/Discard lifecycle.
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

The target compatibility contract requires every completed result to identify
at least:

- engine and result-schema versions;
- strategy source/artifact hash and strategy API version;
- Run Configuration and numeric-policy versions;
- Release Snapshot, Data Segment, and calendar hashes;
- Python runtime profile when Python executed;
- `canonicalResultHash` over canonical functional records.

The implemented internal schema-3 writer adds exact Python source and source hash, canonical parameters,
Strategy API/template versions, runtime profile ID and manifest hash, Python
and package versions, seed, numeric-policy version, and structured diagnostic
provenance. These values are part of the schema-3 canonical identity. The
separate reproducibility-check record references three exact attempt Result
IDs and their status/hash; it does not rewrite any attempt's canonical records
or hash. The schema-2 and schema-3 readers use their separate frozen
framings. The internal schema-2 copy-migration operation validates the source
and its data references, creates a new validated schema-3 Result, preserves
the original, and marks unavailable legacy fields explicitly. The Results UI
action for migration, signed installer/release-baseline verification, and
manual macOS edit-to-Replay acceptance remain planned.

Implemented result schema 2 is a limited predecessor: it records the current
strategy ID/hash, fixed policy literals, data-selection identities, spans, and
supported records, but not every target identity above. Its exact current hash
framing is fixed in
[`EngineReplayPnL.md` §8.1](EngineReplayPnL.md#81-implemented-schema-2-canonical-hash-framing).

Wall-clock creation time, local paths, and SQLite page layout are not functional determinism inputs.
