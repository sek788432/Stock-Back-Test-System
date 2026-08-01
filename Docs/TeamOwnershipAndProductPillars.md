# Team ownership and the three product pillars

This document describes **how work is split across seven topic owners** and how the **three main user-facing functions** of the application map to concrete components. It is **organizational guidance**, not a replacement for the technical specs.

**Primary technical sources of truth**

- [`Specs/00Overview.md`](Specs/00Overview.md) — scope and flow
- [`Specs/11StockScreenerKLineProduct.md`](Specs/11StockScreenerKLineProduct.md) — product contract: K-line replay, strategy modes, screener
- [`Specs/02FrontendQt.md`](Specs/02FrontendQt.md), [`Specs/04DataLayer.md`](Specs/04DataLayer.md), [`Specs/05StrategyAuthoring.md`](Specs/05StrategyAuthoring.md), [`Specs/07EngineReplayPnL.md`](Specs/07EngineReplayPnL.md)
- [`Specs/10CiDevFlow.md`](Specs/10CiDevFlow.md), [`Specs/09BuildDistributionLauncher.md`](Specs/09BuildDistributionLauncher.md)

---

## 1. Parallel work rules (no hard gating between owners)

These rules keep seven streams moving **without** “you must wait for my PR first.”

1. **Contract-first** — Table shapes, C++ interfaces, strategy hooks, and CI policy are fixed in **specs + short interface notes** before (or in parallel with) full implementations.
2. **Replace the stub, not the plan** — Each owner tests against **in-memory bars**, **fake `BarStream`**, **frozen fixtures**, or **mock view-models** until production wiring lands.
3. **Integration is swapping implementations** — The same façade (e.g. data read API, `IStrategy`, `ReplaySessionVm`) gains a real backend when ready; other owners do not block on that day.
4. **Respect repo invariants** — e.g. C++ does not write `StockData/MarketData.duckdb` (see [`Governance/AGENTS.md`](Governance/AGENTS.md) H2); engine **determinism** when semantics change (see `07` §8).

---

## 2. Seven topic owners (what each one owns)

Owner numbers are **labels**, not hierarchy. Each row is a **single accountable theme**.

### Owner 1 — Data acquisition and cleansing (Python pipeline writer)

- `DataFetcher/` jobs: new sources, scheduling entrypoints, validation.
- **Fundamental and reference data** pipelines (market cap, sector, company metadata, etc.) as agreed in the storage contract.
- **Quality checks**: missing fields, calendar alignment, units.
- **Frozen sample exports** (small DuckDB/CSV slices) so consumers never wait for full historical backfill.

### Owner 2 — Storage contract and read model (schema authority for the app)

- Canonical **schema contract**: OHLCV tables, symbol dictionary, fundamental attachment tables, universe definitions, `as-of` semantics.
- **Schema versioning** and migration story (Python side owns writes; see `04`).
- **C++ read path** design: `BarStream`, discovery, errors on drift — implemented or specified so Owner 3–5 can code against it.
- Contract tests (schema vs documentation) that do **not** require live downloads.

### Owner 3 — Engine core (bars → orders → fills → portfolio → metrics)

- `Engine::run`, broker / fill model, `Portfolio`, **batch backtest** results.
- **Replay** pacing: `Replay`, `ReplayClock`, optional seek/checkpoints (`07`).
- **Metrics** computation (`bteMetrics` / equivalent).
- **Determinism harnesses** using **synthetic bar sequences** (no DuckDB required for unit tests).

### Owner 4 — Strategy and scripting (rules, Lua, Python slot, NL boundary)

- `IStrategy`, `OrderBuilder`, `Context` / `InitContext` usage.
- **Rule mode**: `*.rule.json`, `compileRule`, `conditionLogic` (`all` / `any`), indicator binding (`05`, `06`).
- **Lua**: sandbox, `compileLua`, `bte.apiVersion`.
- **Python** (product-facing): `compilePython` and host binding **per ADR**; `pythonApiVersion` in persistence (`05` §7).
- **Natural language**: generate **candidates** only; **explicit user acceptance** before compile/run; audit fields in `.meta.json` (`05` §6, `11` §3).
- **Screener predicate semantics** (shared operators with rules or a documented subset; Python/RULE screener scripts per `11` §2).

### Owner 5 — Desktop client (Qt UI and UX)

- Tabs: Strategies, Backtest, **K-line Replay**, **Screener**, Plugins, Logs (`02`).
- Charts: candlesticks, volume, equity overlays, trade markers; `IChartView` implementation.
- **MVVM bindings** (`ReplaySessionVm`, backtest/screener view-models) — may run on **fake backends** first.
- Accessibility, themes, user-visible errors.
- Qt Test smoke tests (e.g. offscreen) per major surfaces.

### Owner 6 — CI, branch protection, and quality gates

- Workflows: format, static analysis, `ctest`, `pytest`, optional UI smoke (`10`).
- **Branch protection**: required checks green before merge; **docs-only** paths or labels that skip the heavy matrix where policy allows.
- Non-docs PRs: tests that exercise **changed behavior** (anti-cheat / mutation policy per existing ADRs).
- **Frozen fixtures** in-repo so CI never depends on live data pulls.

### Owner 7 — Release, packaging, and install ergonomics

- CMake presets, vcpkg story, per-OS artifacts (`09`).
- **Launcher**: version side-by-side, `active.json`, download/verify if applicable.
- **Developer path**: clone + documented build (single entry in onboarding).
- **End-user path**: GitHub Releases assets (or Launcher); avoid forcing full source builds for non-developers.

---

## 3. The three main product functions (detailed distribution)

The product pillars are defined in [`Specs/11StockScreenerKLineProduct.md`](Specs/11StockScreenerKLineProduct.md). Below: **components per pillar** and **which owner** is accountable. Use **mocks and contracts** so no pillar waits on another stream.

### Pillar A — K-line replay (single-symbol candle playback)

**User intent:** Choose **symbol**, **timeframe (schema)**, **date range**, **initial capital**; play history bar-by-bar with candlestick + volume, buy/sell markers, portfolio strip, and trade log; **same engine semantics** as batch backtest (`07`, `11` §1).

| Component | Primary owner | Notes |
|-----------|---------------|--------|
| OHLCV read contract + `BarStream` for replay session | 2 | Owner 1 feeds data; Owner 2 defines **read** contract. |
| `Replay`, `ReplayClock`, seek/checkpoints, determinism vs batch | 3 | Test with **memory streams**. |
| `IStrategy` invoked per bar during replay | 4 | Same strategy artifact as backtest. |
| Replay tab UI, chart, markers, `ReplaySessionVm` | 5 | Can bind to mock engine first. |
| CI: replay/step fixtures, small bar sets | 6 | No live network. |
| Installable app + Launcher compatibility | 7 | Independent of feature completeness for “first runnable build.” |

### Pillar B — Strategy input and backtest results

**User intent:** Three authoring modes — **built-in conditions (AND/OR)**, **Python script**, **natural language → user-accepted artifact**; **Run backtest** → equity curve, KPIs, trade statistics (`05`, `11` §3).

| Component | Primary owner | Notes |
|-----------|---------------|--------|
| Rule JSON schema, `compileRule`, `RuleStrategy` | 4 | `conditionLogic` per `05` §3. |
| Lua sandbox, `compileLua`, API surface | 4 | Reference embedded host today. |
| Python host, `compilePython`, ADR | 4 | Behavioral spec in `05` §5. |
| NL flow: preview, accept, audit metadata | 4 (logic) / 5 (widgets) | No auto-run without accept (`11` §3.2). |
| `Engine::run`, broker, `BacktestResult`, metrics | 3 | UI displays results via bindings. |
| Backtest tab: curves, tables, summary | 5 | |
| Data range + symbol pick (read-only) | 2 | Owner 1 supplies volume. |
| CI: rule/Lua golden parity; determinism dumps; Python when shipped | 6 | |

### Pillar C — Stock screener / selection

**User intent:** Same three modes as strategies for **universe filtering**; **ALL / ANY** for built-in rows; optional Python screener script; NL → accepted script or rules; results table and export (`11` §2).

| Component | Primary owner | Notes |
|-----------|---------------|--------|
| Fundamental / reference **ingestion** | 1 | Behind published schema. |
| Multi-symbol read APIs, `as-of`, universe views | 2 | C++ stays read-only vs DuckDB. |
| Predicate model (reuse rule operators / script contract) | 4 | Keeps screening semantics aligned with `05`. |
| Scan runner performance (bulk bar walks, caching) | 3 *or* 4 via thin API | Decide in ADR: e.g. “semantic owner 4, throughput owner 3” — **interface** separates them so neither blocks. |
| Screener tab UI, presets persistence (`02` §2.4) | 5 | `<userData>/screeners/` in `02`. |
| CI: tiny universe fixtures, expected pass/fail sets | 6 | |

---

## 4. Cross-cutting commitments (everyone)

- Follow [`Specs/`](Specs/README.md) for the subsystem you touch; product surface for the three pillars is pinned in **`11`**.
- **No exceptions across modules**: `Result<T, Error>` at boundaries (`03`), **deterministic** engine outputs where required (`07` §8).
- **Dependency adds**: ADR + `Decisions/Dependencies.md` per [`Governance/AGENTS.md`](Governance/AGENTS.md).

---

## 5. Evolution

When ownership or pillar scope changes:

- Update **this doc** in the same PR that changes behavior or responsibilities.
- If a decision is architectural (e.g. Python host shape, screener runner placement), add or amend an ADR under [`Decisions/`](Decisions/README.md).

---

*Last aligned with specs `00`, `02`, `04`, `05`, `07`, `09`, `10`, `11`.*
