# Frontend (Qt)

> **Status:** Partially implemented. The current application contains Qt tab,
> chart, replay scaffolding, and a Backtest page for the limited engine slice
> documented in [`EngineReplayPnL.md`](EngineReplayPnL.md) §1. That page supports the fixed starter strategy and the
> implemented Selectable Conditions subset; saved-strategy authoring, Python
> strategies, complete backtest workflows, the complete Results library, and
> release-quality UX remain planned unless their owning spec says otherwise.
> The current shell exposes Strategies, Backtest, Results, and Replay;
> Strategies and Results are placeholders for their accepted future pages.
> Completed runs can nevertheless be persisted and handed directly to Replay.

This spec owns the Qt presentation interface: every screen the user sees, chart
presentation, and how the UI communicates with backend modules without blocking
or acquiring engine authority.

---

## 1. Framework choice — Qt 6 Widgets

The checked-in development baseline requires **Qt 6.8+** with **Widgets** and
currently uses **Qt Charts**; the current CI workflow installs Qt 6.9.x.
Version claims must follow
those files rather than an unpinned "current LTS" label.

### Why Widgets, not QML?

| Criterion | Widgets | QML / Qt Quick |
|---|---|---|
| Native look on desktop (titlebars, menus, file dialogs) | ★★★★★ | ★★★ (needs styling) |
| Engineer ramp-up if you already know C++ | ★★★★★ | ★★★ (JS + property bindings) |
| Tooling on all 3 OSes (Designer, Creator) | mature | mature |
| Performance for tabular data and forms | ★★★★★ | ★★★★ |
| Easy maintenance for one developer | ★★★★★ | ★★★ (JS + C++ split is more files) |

For a single-developer desktop tool that is form-heavy (strategy editor,
dashboards, dialogs), **Widgets wins on maintenance**. The current replay uses
Qt Charts inside a `QChartView`; no repository benchmark establishes an
animation or point-count performance contract.

### Chart direction

Qt Charts remains an implemented development baseline but cannot ship in the
Apache-2.0 distribution under its GPL/commercial terms. The accepted release
direction is a project-owned `QPainter` implementation behind `IChartView`.
It uses a 70/30 candlestick-to-volume split, initially presents 120 bars, and
caps the visible window at exactly 1,000 bars. The replacement must land with
functional, accessibility, and performance tests before distribution.

---

## 2. Window structure

```
┌───────────────────────────────── stockBacktester ─────────────────────┐
│ File   Strategy   Data   View   Help                                  │
├───────────────────────────────────────────────────────────────────────┤
│ ┌─ Tabs ──────────────────────────────────────────────────────────┐   │
│ │ [Strategies] [Backtest] [Results] [Replay]                   │   │
│ ├─────────────────────────────────────────────────────────────────┤   │
│ │                                                                 │   │
│ │                  (active tab content)                           │   │
│ │                                                                 │   │
│ └─────────────────────────────────────────────────────────────────┘   │
│ status bar  │  symbol: AAPL  │  schema: ohlcv-1h  │  rows: 25,431     │
└───────────────────────────────────────────────────────────────────────┘
```

### 2.1 Strategies tab

A two-pane view contains a saved Strategy library and editor. The library and
storage contract are defined by
[`BacktestReplayProduct.md`](BacktestReplayProduct.md). Editor modes are:

1. **Selectable Conditions** (default) — form-driven rows for typed indicators,
   supported fields, comparisons, whole-share sizing, and current actions with
   flat **ALL / ANY** logic. Each buy and sell group supports one to five rows.
   The saved artifact is the versioned typed plan owned by
   [`StrategyAuthoring.md`](StrategyAuthoring.md), not generated Python.
2. **Python Script Strategy** — code editor with diagnostics and **Validate
   Strategy**, using the authoring contract rather than a UI-specific compiler
   interface.

At widths of at least 1100 px, buy and sell groups are side by side; narrower
layouts stack them. A group scrolls vertically inside its panel when its content
exceeds the larger of 420 px or 40% of the viewport. Headers and Add controls
remain sticky; rows are compact and keyboard-reorderable; horizontal scrolling
is forbidden. Nested groups, portfolio gates, generated Python, and **Edit as
Python** are outside scope.

**Use in Backtest** fills the Backtest page with the selected immutable Strategy
version. It does not start a run. Replay is entered from a stored Result.

### 2.2 Backtest tab

The implemented page exposes symbol, range, capital, and whole-share quantity
controls plus a strategy selector. The fixed starter strategy remains available;
the Selectable Conditions option provides flat ALL/ANY buy and sell groups over
bar fields, close-change percentage, and the implemented indicator catalog. It
reports the run status, cash, position,
market value, equity, P&L, processed-bar count, and every fill in a multi-row
trade log. Each range calendar provides a directly selectable year dropdown
alongside the month control instead of requiring repeated previous/next
navigation. This is an incremental implementation of the layout below; saved
Run Configurations, the equity curve, complete metrics, and sortable/proxy-model
trade-log behavior remain planned.

| Region | Content |
|---|---|
| Top toolbar | Universe, range, timeframe, capital, costs, and Strategy from the Run Configuration; **Run** starts the default single active Backtest. |
| Center | Equity curve (`QLineSeries`) and optional presentation-only buy-and-hold overlay; benchmark alpha/beta remains deferred. |
| Right side panel | Summary metrics: total return, CAGR, Sharpe, max drawdown, win rate, # trades, exposure. |
| Bottom | Sortable trade log table (`QTableView` + `QSortFilterProxyModel`). |

**Cross-link.** A completed or diagnostic stored Result has a dedicated **Open
Replay** action.

### 2.3 Replay tab — current baseline and target result playback

The implemented baseline is a bar-only player keyed by symbol, timeframe/range,
and placeholder initial capital. It has no Strategy, fills, or accounting.
Hourly CSV and daily aggregation are the only truthful data paths; other
displayed timeframe choices currently fall back to hourly data and are a known
gap, not supported behavior.
The current loader also turns a load failure into an empty bar collection; the
typed failure behavior in §3 is planned and must land before empty data can be
distinguished reliably from an error.

The target **K-line Replay** opens a validated Backtest Result and its referenced
Data Segments. It does not accept a new Strategy or Run Configuration and does
not invoke Python or the C++ engine. Speed, scrub, portfolio, and marker controls
present persisted records only.

The headline UX. Layout:

```
┌─────────────────────────────────────────────────────────────────────┐
│  Symbol [AAPL ▼]   Speed [1× 5× 10× max]   ◀◀ ⏸ ▶ ▶▶   ███████░░░  │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│        ▲                                                            │
│        │       ╷  ╷  ╶─                                             │
│        │   ╷╷  │  │     ▼ Sell @ 187.20                             │
│        │  ╶┤│ ╶┤  │                                                 │
│        │   ││  │  │     ▲ Buy  @ 173.40                             │
│        │   │   │  │                                                 │
│        └──────────────────────────────────────────────────►         │
│                                                                     │
├──────────────────┬──────────────────────────────────────────────────┤
│  Cash:  $9,830   │   Equity curve so far  ╱╲╱╲╱╲                    │
│  Held:  20 AAPL  │                                                  │
│  Mkt:   $3,744   │   Realized P/L:  +$214.30                        │
│  Total: $13,574  │   Unrealized:    -$ 12.10                        │
└──────────────────┴──────────────────────────────────────────────────┘
```

Interactions:
- **Speed control** — 1× presents one persisted slice per second; max presents
  records without intentional delay. It does not pace engine execution.
- **Pause / Step** — `Space` pauses/resumes presentation; `→` presents the next
  persisted slice.
- **Scrub bar** — seek within validated persisted checkpoints/records and
  referenced bars; seeking never reconstructs fills by rerunning the engine.
- **Trade markers** — green up-triangle for buys, red down-triangle for sells, click to see fill details.
- **Cursor inspector** — hover any candle to see OHLCV + active indicators in a side popup.
- **Volume pane** — histogram under the candles, sharing the visible window.

### 2.4 Results tab

The Results page is separate from Strategies. Its proxy-model table shows the
stored result's Strategy, universe, timeframe, range, status, completion time,
valid total return, result-schema version, canonical-hash state, and data
availability. Actions are Show Details, Open Replay, Compare, Export, Import,
Move to Trash, and Restore. Filters cover Strategy, universe/symbol, timeframe,
status, and date, with separate Active and 30-day Trash views.

Imports are untrusted data. The UI validates them without executing embedded
source. **Open Replay** remains disabled with a structured reason when the
result or referenced data is invalid or unavailable.

---

## 3. View-model seam

- **Implemented baseline:**
  [`ReplaySessionVm`](../../Src/Bindings/Include/Bte/Bindings/ReplaySessionVm.h)
  is a synchronous, dependency-light adapter over the current bar-only Replay.
  It owns the backend with `std::unique_ptr`; its portfolio values are explicitly
  placeholders, not engine accounting.
- **Target:** Qt-facing adapters receive immutable progress/result values from
  an engine worker through queued delivery. Views never hold backend pointers or
  call widgets from workers.
- Fixed-point `Money`, `Price`, and `Quantity` remain authoritative through the
  engine/result seam. Conversion to display numbers or strings occurs only in
  the presentation adapter.
- Empty successful values and typed failures remain distinct; adapters surface
  structured errors instead of substituting empty charts or placeholder state.

---

## 4. Chart abstraction

The implemented development
[`IChartView`](../../Src/Frontend/Include/Bte/Frontend/IChartView.h) seam has
two operations: replace the visible bar window and append one bar.
`QtChartsCandlestickView` is its current adapter. The accepted release
adapter is project-owned `QPainter`; it must not leak rendering types into the
backend or alter persisted ordering.

Indicator overlays, persisted fill/corporate-action markers, crosshair state,
and result seeking are planned interface additions. Add them only with their
real callers, immutable value types, and tests; K-line Replay consumes persisted
result values rather than asking the chart to execute engine logic.

---

## 5. Persistence

| What | Location contract | Format/status |
|---|---|---|
| Strategies | `QStandardPaths::AppLocalDataLocation/Strategies/{Active,Trash}` | UUIDv7 `.btestrategy` typed plan or Python source plus versioned metadata |
| Backtest Results | `QStandardPaths::AppLocalDataLocation/Results/{Active,Trash}` | Planned UUIDv7 `.bteresult`; no durable result format is currently implemented |
| Data and runtimes | `QStandardPaths::AppLocalDataLocation/{DataSegments,RuntimeProfiles}` | Immutable referenced content |
| Settings and layout | OS-appropriate application data directory | Planned UI-only state; never a mutable market-data path |

One future Core path resolver owns these locations and is redirectable in tests.
Writers stage, validate, flush, and atomically rename artifacts on the same
filesystem.

---

## 6. Theming

Light/dark theming, QSS resources, and matching Qt Charts themes are planned.
Exact resource paths are not a public contract until the files exist.

---

## 7. Internationalization

Wrap all user-visible strings in `tr(...)`. Translation catalogs are planned;
their exact paths are not a current contract.

---

## 8. Accessibility

- Every widget has `accessibleName`.
- Every action is keyboard-accessible. Replay shortcuts include `Space`
  play/pause and arrow-key stepping.
- Color choices for buy/sell markers also use shape (▲/▼) so colorblind users still distinguish.

---

## 9. Tests

Current registered frontend tests cover the application shell, current Backtest
page, bar-only Replay page/state, Qt Charts adapter, and Bindings view models.
They do not prove saved Strategy/Result libraries, the five-row editor, Python
authoring, or the project-owned release chart.

Those accepted future behaviors require positive, negative, and boundary tests
for one/five/six condition rows, invalid conditions, responsive layout, inner
scroll threshold, keyboard reorder, Python validation, Result import,
hash/schema failures, Trash/Restore, atomic recovery, and **Open Replay**
eligibility. The release chart also requires deterministic render-geometry,
interaction, and accessibility tests; visual inspection alone is not evidence.
