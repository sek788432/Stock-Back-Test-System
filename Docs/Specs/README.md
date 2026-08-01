# Specs — Stock Back-Test System (C++ Desktop App)

This folder is the design source of truth for the C++/Qt desktop backtester being built on top of the existing Python data pipeline ([`../../DataFetcher/`](../../DataFetcher/README.md), `../../StockData/`).

Read **`00Overview.md` first**. It sets scope, draws the architecture diagram, and tells you which detailed spec covers what. Each subsequent file goes one level deeper on a single subsystem.

**Team ownership** (seven topic owners × three product pillars) is described in [`../TeamOwnershipAndProductPillars.md`](../TeamOwnershipAndProductPillars.md) — organizational only; specs remain authoritative for behavior.

| File | One-line summary |
|---|---|
| [`00Overview.md`](00Overview.md) | Purpose, end-to-end flow, repo layout, NFRs |
| [`01Architecture.md`](01Architecture.md) | Module dependency graph, threading, error model, build presets |
| [`02FrontendQt.md`](02FrontendQt.md) | Qt UI, charts (incl. K-line replay), screener, MVVM |
| [`03BackendCore.md`](03BackendCore.md) | `Bar`, `Order`, `Trade`, `Portfolio`, `Result<T,Error>`, naming |
| [`04DataLayer.md`](04DataLayer.md) | DuckDB + CSV adapters, `BarStream`, prefetch, caching |
| [`05StrategyAuthoring.md`](05StrategyAuthoring.md) | Rule DSL (JSON) + Lua sandbox + **Python / NL roadmap**, all target `IStrategy` |
| [`06Indicators.md`](06Indicators.md) | Streaming TA library, full Phase-1 catalog |
| [`07EngineReplayPnL.md`](07EngineReplayPnL.md) | Backtest loop, broker simulator, replay clock, metrics |
| [`08PluginSystem.md`](08PluginSystem.md) | Native C++ plugin ABI + Lua, SDK packaging, trust model |
| [`09BuildDistributionLauncher.md`](09BuildDistributionLauncher.md) | CMake, vcpkg, CI matrix, per-OS packaging, **Launcher** |
| [`10CiDevFlow.md`](10CiDevFlow.md) | PR gates, mandatory tests for every symbol, **anti-cheat audit**, mutation testing |
| [`11StockScreenerKLineProduct.md`](11StockScreenerKLineProduct.md) | Product contract: **K-line replay**, **three strategy input modes**, **stock screener** (same modes + AND/OR) |
| [`12AiActionRouter.md`](12AiActionRouter.md) | CLI skills + Chrome extension → action JSON → Qt parser. Realises Spec 11 §3 NL mode without LLM credentials. |

## Decisions baked in

These were chosen up-front to keep the rest of the design simple:

- **Qt 6 LTS, Widgets + Qt Charts** for the UI (`02` explains why over QCustomPlot or custom QPainter).
- **Hybrid strategy authoring**: rule-based JSON for the form-driven editor, Lua 5.4 (sandboxed, via sol2) for advanced scripts. Both compile to one `IStrategy` interface (`05`). **Product UX** also targets **Python** and **natural-language → script** paths (`11`); binding Python to `IStrategy` is an implementation choice recorded in an ADR when implemented.
- **DuckDB read-only** from C++; the existing Python pipeline keeps owning writes (`04`).
- **Launcher app** for version management — users install once, then any number of app versions live side-by-side under `<userData>/versions/`. `active.json` selects the current one (`09`).
- **Determinism is mandatory** for engine output across OSes and runs (`07`).
- **Native plugins** are full-trust, but each load is hash-confirmed by the user (`08`).
- **Naming**: `lowerCamelCase` for variables/methods, `UpperCamelCase` for types, and PascalCase for project-owned path components. Unit suites live under `Tests/Unit/<Module>/`; see ADR 0010 for conventional exceptions.
- **CI is the merge gate, not the reviewer** — every public symbol must have a test, every test is checked against a defined "no-cheating" rulebook, and mutation testing forces tests to actually catch bugs (`10`).

## What's intentionally not in here yet

- The **full** CMake/vcpkg/Qt tree from [`09`](09BuildDistributionLauncher.md). A minimal bootstrap (Core + tests) lives at repo root; see [`../BUILD.md`](../BUILD.md).
- Full database migration story for breaking schema changes — Python pipeline owns that, and current schema is stable.
- Live trading. The whole system is designed assuming historical bars only. Live feeds would extend the `BarStream` interface but are out of scope until backtest + replay land.
- Cloud sync. Strategies and settings are local-only by design.
- **Stock screener** wire-up to all data sources: `11` states the product contract; batch performance and universe sources may land incrementally (`04`).

## How to evolve these specs

- One PR = one spec change. Comment trail lives on the PR.
- Update `00Overview.md` if scope or flow changes; update the relevant detail spec for everything else.
- Numbers (perf targets, defaults) are non-binding suggestions — adjust as we measure.
- Anything in **`07`** that changes engine semantics needs a determinism-fixture refresh in CI.
