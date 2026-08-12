# 00 — Overview and Flow

This is the entry point for the desktop backtester. Detailed module contracts live in Specs `01`–`12`; [ADR 0011](../Decisions/0011-own-the-engine-and-release-data-contract.md) records the governing architecture.

## 1. Product

The planned product is a cross-platform C++20/Qt desktop application for a single local user to:

- Author a Strategy through Selectable Conditions or a Python Script Strategy.
- Run both modes through the same project-owned C++ backtest engine.
- Inspect orders, fills, positions, costs, margin events, metrics, and diagnostics.
- Replay a completed or diagnostic Backtest Result against its referenced K-line data.
- Screen a selected universe and explicitly accept any AI-proposed strategy artifact before execution.

Natural language is an authoring assistant, not an execution mode. Native plugins are a separate extension mechanism; they do not replace the engine.

## 2. Delivery status

| Status | Meaning |
|---|---|
| **Implemented** | Present in the repository and covered by the current registered tests. |
| **Planned** | Accepted contract; implementation or complete verification has not landed. |
| **Blocked** | Must not ship until the named prerequisite is resolved. |

### Implemented

- C++20 Core, CSV-backed data loading, a basic bar-step replay, Qt replay UI, and JSON replay-summary persistence.
- A limited C++ starter Engine and Backtest page: one market buy submitted on
  the first bar, execution at the next actual bar open, fixed slippage,
  affordability rejection, and final-close open-position marking.
- `DataFetcher/` developer pipeline for Databento ingestion into DuckDB and tracked CSV extraction.

### Planned

- General canonical C++ strategy execution, synchronized `MarketSlice`, broker
  and margin modeling, complete accounting/metrics, and deterministic event
  sequencing beyond the limited starter run.
- Typed C++ Selectable Conditions and an isolated trusted Python worker using the same engine.
- Immutable release snapshots generated from `StockData/Extracted`.
- Fixed-point accounting and transactional SQLite `.bteresult` files.
- Complete K-line result replay, screening, packaging, and release-profile retention.

### Blocked

- **Public data distribution:** no recorded right currently permits redistribution of the Databento-derived tracked extracts.
- **Split correctness:** no verified, redistribution-cleared split manifest currently accompanies the bars.

Neither blocker may be softened into a warning for a public release.

## 3. Canonical flow

1. Release CI validates tracked extracts and, after the blockers are cleared, builds a hashed immutable Release Snapshot and calendar.
2. The user selects a snapshot, universe, time range, capital, cost profile, and Strategy to create a Run Configuration.
3. Selectable Conditions become a typed C++ plan. A Python Script Strategy runs in a fresh worker and receives chronology-safe slices/history.
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
| [01](01Architecture.md) | Modules, seams, ownership, and processes |
| [03](03BackendCore.md) | Core value types, precision, errors, and statuses |
| [04](04DataLayer.md) | Release snapshots, developer ingestion, and retention |
| [05](05StrategyAuthoring.md) | Selectable Conditions and Python strategy contract |
| [07](07EngineReplayPnL.md) | Event sequence, execution, accounting, metrics, and replay |
| [10](10CiDevFlow.md) | Checks that are implemented versus planned |
| [11](11StockScreenerKLineProduct.md) | User-facing replay and screener behavior |

Deeper specs refine this overview but may not contradict it. If two specs overlap, the spec named in this table owns the concept.
