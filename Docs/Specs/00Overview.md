# 00 — Overview & Flow

This document is the entry point for the C++ desktop backtester specs. It describes **what** the system is, **who** uses it, and **how** the pieces talk to each other. Detailed module specs (`01`–`11`) link off this one.

---

## 1. Purpose

A cross-platform (Windows / macOS / Linux) desktop application that lets a single local user:

1. **Author** a trading strategy in **three complementary ways** (`05`, `11`):
   - **Built-in components** — form-driven rules (JSON) with indicators, thresholds, and portfolio gates.
   - **Python script** — advanced, code-first strategies with validation and sandboxing (`11` §3).
   - **Natural language** — an AI-assisted path that **proposes** Python or structured rules; the user **accepts** before compile/run (`11` §3).
   *Implementation note:* Lua 5.4 remains the reference embedded script host in `05` until a Python host lands; product-facing docs standardize on **Python** as the user’s script language.
2. **Backtest** that strategy over historical OHLCV bars stored in DuckDB.
3. **K-line replay** — for a chosen **symbol**, **timeframe (schema)**, and **date range**, play history bar-by-bar on a candlestick chart with the **same engine** as batch backtest: buy/sell markers, volume, cash, positions, equity, trade log (`07`, `11` §1).
4. **Screen / select stocks** across a universe using the **same three modes** as strategy authoring (built-in conditions with **AND/OR**, Python, natural language → script) and inspect tabular results (`11` §2).
5. **Inspect** results — equity curve, trade log, summary metrics.
6. **Extend** the system through plugins (C++ shared libraries) and script strategies.
7. **Stay current** through a small Launcher app that downloads new releases from GitHub and lets the user switch between installed versions.

The Python pipeline already in this repo (`DataFetcher/`) is the **only** writer to the DuckDB file. The C++ app is a **read-only** consumer of `StockData/MarketData.duckdb` and the `StockData/Extracted/*.csv` snapshots.

---

## 2. High-level architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Desktop Application                         │
│                                                                     │
│  ┌──────────────────────────┐        ┌───────────────────────────┐  │
│  │     Qt Frontend (UI)     │        │      Launcher (Qt)        │  │
│  │  - Strategy editor       │        │  - List installed         │  │
│  │  - Backtest + screener   │        │  - Download from GitHub   │  │
│  │  - K-line replay         │        │  - Switch active version  │  │
│  │  - Metrics dashboard     │        └───────────────────────────┘  │
│  └────────────┬─────────────┘                                       │
│               │  Q_OBJECT bindings  (signals/slots)                 │
│  ┌────────────▼────────────────────────────────────────────────┐    │
│  │                    Backend Core (pure C++)                  │    │
│  │                                                             │    │
│  │  data/  ──►  indicators/  ──►  strategy/  ──►  engine/      │    │
│  │   ▲              ▲                ▲              │          │    │
│  │   │              │                │              ▼          │    │
│  │   │              │                │          metrics/       │    │
│  │   │              │                │              │          │    │
│  │   │              │                │              ▼          │    │
│  │   │              │                │      ┌───────────────┐  │    │
│  │   │              │                │      │  Portfolio /  │  │    │
│  │   │              │                │      │  Broker sim   │  │    │
│  │   │              │                │      └───────────────┘  │    │
│  │   │                                                         │    │
│  │   │              plugins/   ◄── scripts (Lua/Python*) / .so ─┐  │    │
│  └───┼─────────────────────────────────────────────────────┼──┘    │
│      │                                                     │       │
└──────┼─────────────────────────────────────────────────────┼───────┘
       │                                                     │
       ▼                                                     ▼
  ┌─────────────────────┐                       ┌──────────────────────┐
  │ StockData/          │                       │  ~/.stockBacktester/ │
  │  MarketData.duckdb  │  (read-only)          │   plugins/  configs/ │
  │                     │                       │   strategies/        │
  └─────────────────────┘                       └──────────────────────┘
```

The arrows in `data → indicators → strategy → engine → metrics` show the **bar-event flow**, not a build dependency. Each module exposes a stable C++ interface (header) so it can be unit-tested independently and so the Qt layer never reaches into engine internals directly.

---

## 3. End-to-end flow (typical session)

1. **Launch.** Launcher (`09`) verifies the active version, then starts `stockBacktester` from that version's folder.
2. **Load data.** UI lets the user pick a symbol or basket from DuckDB. The `data/` layer (`04`) opens `MarketData.duckdb` read-only and exposes a `BarStream` per symbol.
3. **Author strategy.** User picks **built-in (rule)**, **Python**, or **natural language** mode (`05`, `11`):
   - **Built-in** — form rows and optional **ALL/ANY** logic among conditions where the schema supports it (`11` §2.2; rule structure in `05` §3).
   - **Python** — writes a script against the strategy context API (`05` §5).
   - **Natural language** — assistant drafts script or rules; user reviews and accepts.
     The editor compiles/validates on demand and reports errors inline.
4. **Backtest.** User clicks **Run**. The `engine/` (`07`) iterates the `BarStream`, hands each bar to `indicators/` (`06`) and the strategy, the strategy emits orders, the broker simulator fills them on the **next bar's open** (configurable), and a `Portfolio` updates cash/holdings.
5. **Inspect.** Equity curve, drawdown, Sharpe, win-rate, trade list — all rendered in the dashboard.
6. **Replay (K-line).** User opens **Replay** with **symbol**, **timeframe**, **start/end**, and **initial capital** (`11` §1). Same engine and strategy as backtest, driven by a `ReplayClock` so bars emit at user-controlled speed. Chart shows candles and volume; buy/sell markers; portfolio strip; trade log. Pause / step / speed presets / scrub per `07` §5.
7. **Screener (optional same session).** User defines universe + conditions (built-in with AND/OR, Python, or NL→script), runs scan, reviews table/export (`11` §2).
8. **Save.** Strategies, replay sessions, screener presets, and result snapshots persist to `~/.stockBacktester/` (per-user, OS-appropriate path — see `09`).

---

## 4. Key non-functional requirements

| Concern          | Target                                                                                                                                                                               |
| ---------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **Platforms**    | Windows 10+ (x64), macOS 12+ (arm64 + x64), Linux glibc 2.31+ (x64). Single CMake build per OS/arch.                                                                                 |
| **Languages**    | C++20 (backend, Qt UI), Lua 5.4 (embedded strategy scripting reference; `05`), **Python** (user-facing script strategies + screeners per `11`; host binding via ADR), Python 3.11+ (data pipeline), optional external/local **LLM** for NL authoring only (`11`).                                                                                  |
| **UI framework** | Qt 6 LTS (Widgets + Qt Charts). Rationale in `02`.                                                                                                                                   |
| **Build**        | CMake 3.24+, vcpkg or Conan for third-party deps. One `CMakeLists.txt` tree, presets per OS.                                                                                         |
| **Naming**       | C++ identifiers: **lowerCamelCase** for variables, methods, free functions; **UpperCamelCase** for classes, structs, enums. Project-owned file stems and directories use **PascalCase**; unit tests use `UnitTest_<Thing>` and live under `Tests/Unit/<Module>/`. Tool-discovery and domain-data exceptions are recorded in ADR 0010. |
| **Testing**      | GoogleTest (C++), Qt Test (UI), pytest (Python). Every public symbol must have a test; PRs gate on diff coverage, anti-cheat audit, and mutation testing. See `10`.                  |
| **Performance**  | Backtest 10 years of `ohlcv-1h` (~25k bars × 500 symbols) in < 30 s on a modern laptop. Replay can pump 60 bars/s without UI lag.                                                    |
| **Memory**       | Streaming-first. Never load the full DuckDB into RAM; always windowed reads.                                                                                                         |
| **Threading**    | Engine/data run on a worker thread; Qt UI is the only thread allowed to touch widgets. Cross-thread comms via `QMetaObject::invokeMethod` / signals.                                 |
| **Distribution** | Self-contained release per OS, signed where possible. GitHub Releases is the source of truth. Launcher manages installs. See `09`.                                                   |
| **Errors**       | All public backend APIs return `Result<T, Error>` (no exceptions across module boundaries). Logging via `spdlog`.                                                                    |

---

## 5. Repository layout (target)

```
Stock-Back-Test-System/
├── DataFetcher/              # existing Python pipeline (unchanged)
├── StockData/                # existing DuckDB + extracted CSVs
├── Docs/                     # all human-facing docs
│   ├── Specs/                # this folder
│   ├── Decisions/            # ADRs
│   └── Governance/           # AGENTS, CONTRIBUTING, LICENSE, CHANGELOG
├── Src/                      # all C++ source
│   ├── App/                  # Qt main app entry + QML/Widgets composition
│   ├── Launcher/             # version manager / updater
│   ├── Frontend/             # Qt views, view-models, charts, editors
│   ├── Backend/
│   │   ├── Core/             # Bar, Order, Trade, Portfolio, Time, Result
│   │   ├── Data/             # DuckDB / CSV adapters, BarStream
│   │   ├── Indicators/       # SMA, EMA, RSI, MACD, BB, ATR, ...
│   │   ├── Strategy/         # rule engine, Lua engine, Python host (planned), plugin loader
│   │   ├── Engine/           # backtest + replay engines, broker sim
│   │   └── Metrics/          # PnL, Sharpe, drawdown, trade stats
│   └── Plugins/              # built-in plugins (sample strategies, etc.)
├── Tests/                    # gtest + qtest
├── Output/                   # CMake binary dir (gitignored); e.g. Output/dev, Output/release
├── ThirdParty/               # vendored single-header libs (or via vcpkg)
├── Resources/                # icons, QSS themes, default Lua scripts
├── CMake/                    # toolchain files, helper modules
├── Packaging/                # NSIS, .desktop, Info.plist, AppImage recipe
└── CMakeLists.txt
```

---

## 6. How to read the rest of the specs

Read in order if you're new:

| #   | File                                | What it answers                                                |
| --- | ----------------------------------- | -------------------------------------------------------------- |
| 01  | `01Architecture.md`                | Module boundaries, dependency graph, threading model           |
| 02  | `02FrontendQt.md`                 | Qt UI: charts, replay (K-line), strategies, **screener**, MVVM  |
| 03  | `03BackendCore.md`                | Core types (`Bar`, `Order`, `Trade`, `Portfolio`), error model |
| 04  | `04DataLayer.md`                  | DuckDB / CSV adapters, `BarStream`, schema discovery           |
| 05  | `05StrategyAuthoring.md`          | Rule DSL, Lua/Python script hosts, NL → accepted artifact      |
| 06  | `06Indicators.md`                  | Streaming indicator API, full built-in catalog                 |
| 07  | `07EngineReplayPnL.md`           | Batch backtest + **K-line replay**, broker model, metrics      |
| 08  | `08PluginSystem.md`               | Native DLL plugins + **script files** (`05`); ABI / trust      |
| 09  | `09BuildDistributionLauncher.md` | CMake, packaging per OS, the version-switching Launcher        |
| 10  | `10CiDevFlow.md`                 | Automated PR pipeline, mandatory tests per symbol, anti-cheat  |
| 11  | `11StockScreenerKLineProduct.md` | K-line replay inputs, 3 strategy modes, stock screener contract   |

When in doubt about scope, this Overview wins; deeper docs refine, they don't override. **Product-facing surface area** for replay, authoring modes, and screening is additionally pinned in `11`.
