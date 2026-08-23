# Overview and Flow

This is the entry point for the desktop backtester. Detailed contracts use
descriptive filenames in the [specification index](README.md); the
[engine and release-data decision](../Decisions/ImportantDecisions.md#engine-and-release-data-authority)
records the governing rationale.

## 1. Product

The planned product is a cross-platform C++20/Qt desktop application for a single local user to:

- Author a Strategy through Selectable Conditions or a Python Script Strategy.
- Run both modes through the same project-owned C++ backtest engine.
- Inspect orders, fills, positions, costs, margin events, metrics, and diagnostics.
- Replay a completed or diagnostic Backtest Result against its referenced K-line data.
- Save reusable Strategies separately from stored Backtest Results.

Native plugins, AI-assisted authoring, a canonical Stock Screener, and advanced
multi-run concurrency are outside the accepted product scope.

## 2. Delivery status

| Status | Meaning |
|---|---|
| **Implemented** | Present in the repository and covered by the current registered tests. |
| **Planned** | Accepted contract; implementation or complete verification has not landed. |
| **Blocked** | Must not ship until the named prerequisite is resolved. |

### Implemented

- C++20 Core, CSV-backed data loading, a basic bar-step replay, Qt replay UI, and JSON replay-summary persistence.
- A limited C++ Engine and Backtest page with a fixed starter strategy and a
  Selectable Conditions strategy: flat ALL/ANY buy and sell groups over bar
  fields, close-change percentage, and the implemented indicator catalog;
  whole-share market orders; next-actual-bar-open
  execution; fixed slippage; affordability rejection; final-close
  open-position marking; and a multi-fill trade log.
- `DataFetcher/` developer pipeline for Databento ingestion into DuckDB and tracked CSV extraction.

### Planned

- General canonical C++ strategy execution, synchronized `MarketSlice`, broker
  and margin modeling, complete accounting/metrics, and deterministic event
  sequencing beyond the limited starter run.
- The complete typed C++ Selectable Conditions contract and an isolated trusted
  Python worker using the same engine. The implemented conditions slice remains
  limited to the indicators, comparisons, sizing, and actions listed above.
- Immutable release snapshots generated from `StockData/Extracted`.
- Fixed-point accounting and transactional SQLite `.bteresult` files.
- Complete K-line result replay, packaging, and release-profile retention.

### Blocked

- **Public data distribution:** no recorded right currently permits redistribution of the Databento-derived tracked extracts.
- **Split correctness:** no verified, redistribution-cleared split manifest currently accompanies the bars.

Neither blocker may be softened into a warning for a public release.

## 3. Canonical flow

1. Release CI validates tracked extracts and, after the blockers are cleared, builds a hashed immutable Release Snapshot and calendar.
2. The user selects a snapshot, universe, time range, capital, cost profile, and Strategy to create a Run Configuration.
3. Selectable Conditions become a typed C++ plan. A Python Script Strategy runs
   in a fresh worker and receives chronology-safe slices/history.
4. The C++ engine alone evaluates orders, applies fills, updates accounting and margin, and produces canonical events.
5. The result writer stages a `.bteresult` file, commits typed records, validates its canonical hash, and atomically promotes it.
6. K-line Replay reads the result and its retained content-addressed Data Segments; it does not rerun a second engine.

## 4. Authority and constraints

- C++ owns market data presented to executable strategies, chronology, execution, portfolio state, and persistence.
- Python may propose commands but cannot mutate engine state or replace bars.
- Runtime downloads, provider credentials, user market-data imports, and arbitrary `pip install` are out of V1 scope.
- Actual rows are used as supplied, including extended-hours hourly bars. Missing bars stay absent.
- Splits require verified metadata; dividends are excluded and every result states that it is price-return rather than total-return.
- Public backend failures use `bte::core::Result<T>`; exceptions do not cross module seams.

## 5. Where details live

| Spec | Authority |
|---|---|
| [`Architecture.md`](Architecture.md) | Modules, seams, processes, and threading |
| [`BackendCore.md`](BackendCore.md) | Core values, precision, errors, and statuses |
| [`DataLayer.md`](DataLayer.md) | Release snapshots, developer ingestion, and retention |
| [`StrategyAuthoring.md`](StrategyAuthoring.md) | Selectable Conditions and Python strategy contract |
| [`EngineReplayPnL.md`](EngineReplayPnL.md) | Event sequence, execution, accounting, metrics, and replay |
| [`CiDevFlow.md`](CiDevFlow.md) | Checks that are implemented versus planned |
| [`BacktestReplayProduct.md`](BacktestReplayProduct.md) | Strategy, Backtest, Result, and Replay workflows |

Deeper specs refine this overview but may not contradict it. If two specs overlap, the spec named in this table owns the concept.
