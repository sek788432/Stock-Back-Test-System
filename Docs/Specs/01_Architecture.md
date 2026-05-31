# 01 — Architecture

How the C++ backend, Qt frontend, plugin layer, and Launcher fit together. This is the contract every other spec implements.

---

## 1. Module dependency graph

Build-time dependencies only (no cycles):

```
                 ┌─────────────┐
                 │     App     │  Qt main(), composes Frontend + Backend
                 └──────┬──────┘
                        │
        ┌───────────────┴───────────────┐
        ▼                               ▼
  ┌───────────┐                  ┌───────────┐
  │ Frontend  │                  │  Engine   │
  └─────┬─────┘                  └─────┬─────┘
        │                              │
        │     ┌────────────┐    ┌──────┴──────┐
        └────▶│  Bindings  │◀───│   Metrics   │
              └─────┬──────┘    └──────┬──────┘
                    │                  │
                    ▼                  ▼
                ┌─────────┐      ┌────────────┐
                │Strategy │      │   Data     │
                └────┬────┘      └─────┬──────┘
                     │                 │
                     ▼                 │
              ┌──────────────┐         │
              │  Indicators  │         │
              └──────┬───────┘         │
                     │                 │
                     ▼                 ▼
                  ┌──────────────────────┐
                  │         Core         │  (Bar, Order, Trade, Result, Time)
                  └──────────────────────┘
```

Rules:
- **Core** has zero internal deps. Only stdlib + `fmt`/`spdlog`.
- **Frontend** never includes anything from `Engine` directly — only through **Bindings** (Qt-aware adapters that own a backend value and expose `Q_PROPERTY` / signals).
- **Plugins** link only against **Core** + **Indicators** + **Strategy** public headers. They never see Engine internals.
- **Launcher** is its own executable. It links **Core** (for `Result`, logging, semver) + **Qt6** (Core, Widgets, Network for UI and downloads). It does not link Engine, Data, Strategy, or other backend modules.

---

## 2. Static / dynamic library breakdown

| Target | Kind | Notes |
|---|---|---|
| `bteCore` | static lib | Core types. Header-mostly, small `.cpp`. |
| `bteData` | static lib | DuckDB adapter; depends on `duckdb` (vendored). |
| `bteIndicators` | static lib | Pure functions on streams. |
| `bteStrategy` | static lib | Rule engine + Lua bridge (links `lua`/`sol2`); **Python host** added per ADR when shipped (`05`, `11`). |
| `bteEngine` | static lib | Backtest + replay loops, broker sim. |
| `bteMetrics` | static lib | PnL, Sharpe, etc. |
| `bteBindings` | static lib | Qt-aware adapters (`Q_OBJECT`). Links Qt::Core. |
| `stockBacktester` | executable | Qt app. Links bindings + frontend + everything. |
| `stockBacktesterLauncher` | executable | Independent updater/launcher. |
| `bte_*_plugin.{so,dylib,dll}` | shared lib | User plugins. Compiled separately, dropped in `~/.stockBacktester/plugins/`. |

`bte` = "BackTest Engine" — short, lowerCamelCase-friendly prefix to avoid symbol collisions and keep target names short on Windows command lines.

---

## 3. Threading model

Three logical threads:

1. **UI thread (Qt main)** — only thread that touches widgets / models. Owns `QApplication`.
2. **Engine worker thread** — runs `Backtest::run()` or `Replay::tick()`. Owned by a `QThread` started from the Frontend. Communicates back via queued signals.
3. **Data I/O thread** *(optional)* — DuckDB queries can block; we run them in a `QThreadPool` task. The `BarStream` returned to the engine reads from a small ring buffer that the I/O thread refills.

Cross-thread rules:
- **No shared mutable state.** `Bar`, `Order`, `Trade` are value types. `Portfolio` is owned by the engine, only **snapshots** (immutable copies) are shipped to the UI.
- All UI updates from worker threads go through `emit signal(...)` with `Qt::QueuedConnection`. Never call widget methods from a worker.
- **Cancellation** is a `std::stop_token` plumbed into `Engine::run` and into embedded script runners (Lua: instruction-count debug hook today; Python: analogous interrupt when host ships — **`05`**).

---

## 4. Error model

No exceptions cross module boundaries. Every public backend function returns one of:

```cpp
template <typename T>
struct Result {
    std::optional<T> value;
    Error error;            // empty when ok
    bool ok() const { return !error; }
};
```

Where `Error` is:

```cpp
struct Error {
    ErrorCode code;             // enum class, namespaced per module
    std::string message;        // human-readable, ready for UI
    std::source_location where; // file:line, for logs
    std::vector<Error> causes;  // optional chain
    explicit operator bool() const { return code != ErrorCode::ok; }
};
```

- **Lua / Python strategy errors** are converted to `Error` at the script/C++ boundary by `bteStrategy` (same codes as **`05`**).
- **DuckDB errors** wrap the DuckDB exception's `what()`.
- **UI** displays `error.message` directly; logs include the chain + file:line.
- Internal helpers may throw; **public APIs never do**.

---

## 5. Configuration & paths

Per-OS user data directory (resolved by `bteCore::userDataDir()`):

| OS | Path |
|---|---|
| Windows | `%APPDATA%\stockBacktester\` |
| macOS | `~/Library/Application Support/stockBacktester/` |
| Linux | `${XDG_DATA_HOME:-~/.local/share}/stockBacktester/` |

Subfolders the app expects to create:
```
<userData>/
├── config/        # settings.json, theme.qss override
├── strategies/    # *.rule.json, *.lua, *.py (+ optional *.meta.json)
├── screeners/     # saved screener presets (`02`, `11`)
├── plugins/       # user-dropped *.so / *.dll / *.dylib (native ABI only — not user `.py`)
├── sessions/      # saved replay sessions
├── logs/          # rotating spdlog files
└── cache/         # query result cache, indicator cache
```

The DuckDB file location is **separate** and tracked in `config/settings.json`. By default it points to the repo's `StockData/MarketData.duckdb`, but the user can rebind via the UI.

---

## 6. Public-vs-internal headers

Each backend module follows:

```
Src/Backend/Foo/
├── Include/Bte/Foo/        # public headers — what other modules see
│   └── PublicApi.h
├── Private/                 # implementation + private headers
│   ├── Internal.h
│   └── PublicApi.cpp
└── Tests/
```

`target_include_directories(... PUBLIC Include INTERFACE Include PRIVATE Private)` so consumers only see `Include/`. Private headers stay invisible.

---

## 7. Build presets

`CMakePresets.json` ships with at least:

| Preset | OS | Generator | Notes |
|---|---|---|---|
| `windows-msvc-x64` | Windows | Visual Studio 17 2022 | dynamic (x64-windows triplet), /W4 /WX |
| `macos-arm64` | macOS | Xcode | universal off; build arm64 + x64 separately |
| `macos-x64` | macOS | Xcode | for older Intel Macs |
| `linux-clang-x64` | Linux | Ninja | clang 17, libc++ optional |
| `dev` | host | Ninja | sanitizers on (asan + ubsan), tests on |

See `09` for the full packaging story.

---

## 8. Stability & compatibility

- **Backend public headers** follow semver. Plugin ABI breaks only on a major bump (see `08`).
- **Lua API** versioned via `bte.apiVersion` global. Plugins read it at load and refuse to run on mismatched majors. **Python** strategies carry `pythonApiVersion` once published (`05` §7 persistence).
- **DuckDB schema** is the Python pipeline's responsibility (`hourlyBars` table). The C++ data layer probes columns at startup and warns on shape changes — see `04`.
