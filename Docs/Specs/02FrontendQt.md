# 02 — Frontend (Qt)

> **Status:** Partially implemented. The current application contains Qt tab, chart, and replay scaffolding. Strategy authoring, complete backtest workflows, screening, plugins, and release-quality UX remain planned unless their owning spec says otherwise.

This spec owns the Qt presentation interface: every screen the user sees, chart
presentation, and how the UI communicates with backend modules without blocking
or acquiring engine authority.

---

## 1. Framework choice — Qt 6, Widgets + Qt Charts

The checked-in CMake contract requires **Qt 6.8+** with **Widgets** and **Qt
Charts**; the current CI workflow installs Qt 6.9.x. Version claims must follow
those files rather than an unpinned "current LTS" label.

### Why Widgets, not QML?

| Criterion | Widgets | QML / Qt Quick |
|---|---|---|
| Native look on desktop (titlebars, menus, file dialogs) | ★★★★★ | ★★★ (needs styling) |
| Engineer ramp-up if you already know C++ | ★★★★★ | ★★★ (JS + property bindings) |
| Tooling on all 3 OSes (Designer, Creator) | mature | mature |
| Performance for tabular data and forms | ★★★★★ | ★★★★ |
| Performance for animated charts at 60 fps | ★★★★ (with Qt Charts + GL backend) | ★★★★★ |
| Easy maintenance for one developer | ★★★★★ | ★★★ (JS + C++ split is more files) |

For a single-developer desktop tool that is form-heavy (strategy editor, dashboards, dialogs) and has one animated view (replay), **Widgets wins on maintenance**. We use Qt Charts inside a `QChartView` for the chart — that one view is fast enough.

### Why Qt Charts (not QCustomPlot, not custom QPainter)?

You asked for "good-looking and easy to maintain". Trade-offs:

| Library | Look | Performance | License | Maintenance |
|---|---|---|---|---|
| **Qt Charts** (chosen) | Modern, themeable, antialiased, dark-mode aware out of the box. Built-in `QCandlestickSeries`, `QLineSeries`, axes, legends. | Good up to ~5–10k visible points; we **only ever render the visible window** (200–500 candles), so this is a non-issue. | LGPL/Commercial — same license as Qt itself. **No new license to manage.** | Same release cadence as Qt LTS; if Qt is supported, Charts is supported. |
| QCustomPlot | Engineering look, less "modern". Excellent. | Excellent — single-header, tens of thousands of points without breaking a sweat. | **GPLv3 or paid commercial.** This is a real liability if you ever want to ship closed-source. | Single maintainer; very mature but third-party. |
| Custom QPainter / Qt Quick Scene Graph | Anything you can paint. | As good as you make it. | Inherits Qt. | **Months of work** for proper trading-grade rendering (axes, crosshair, scaling, perf). Not worth it now. |

**Decision: Qt Charts.** The reasoning is licensing simplicity (no new GPL constraint), zero third-party dependency to track, and the performance ceiling is irrelevant when we always windowed-render. If we ever need pro-grade tick replay (1-minute bars over 10 years all visible), we revisit; the chart layer is hidden behind a `IChartView` interface (see §4) so swapping is a contained change.

We layer simple visual polish on top:
- Custom `QPalette` with two named themes (Light / Dark) loaded from QSS.
- Crosshair, candle hover tooltip, and trade markers drawn as a `QGraphicsItem` overlay on top of `QChartView` — Qt Charts exposes `mapToValue` so this is straightforward.

---

## 2. Window structure

```
┌───────────────────────────────── stockBacktester ─────────────────────┐
│ File   Strategy   Data   View   Help                                  │
├───────────────────────────────────────────────────────────────────────┤
│ ┌─ Tabs ──────────────────────────────────────────────────────────┐   │
│ │ [Strategies] [Backtest] [Replay] [Screener] [Plugins] [Logs]    │   │
│ ├─────────────────────────────────────────────────────────────────┤   │
│ │                                                                 │   │
│ │                  (active tab content)                           │   │
│ │                                                                 │   │
│ └─────────────────────────────────────────────────────────────────┘   │
│ status bar  │  symbol: AAPL  │  schema: ohlcv-1h  │  rows: 25,431     │
└───────────────────────────────────────────────────────────────────────┘
```

### 2.1 Strategies tab

A two-pane view:
- **Left** — list of saved strategies (from `<userData>/strategies/`).
- **Right** — editor for the selected strategy.

Editor has **three visible modes** (two concrete artifact types):

1. **Selectable Conditions** (default) — form-driven rows for typed indicators,
   filters, portfolio gates, sizing, and actions with flat **ALL / ANY** logic.
   The saved artifact is the versioned typed plan owned by Spec 05, not generated
   Python.
2. **Python Script Strategy** — code editor with diagnostics and **Validate
   Strategy**, using the contract in Spec 05 rather than a UI-specific compiler
   interface.
3. **AI candidate import** — provider-neutral preview/diff and explicit
   acceptance. No candidate auto-runs, and V1 contains no direct provider chat
   or credential flow (`12`). Acceptance creates an ordinary conditions or
   Python artifact.

The editor can create a Run Configuration and start a Backtest. **Open in
Replay** is enabled only for an existing Backtest Result; it never executes the
editor's current Strategy.

### 2.2 Backtest tab

| Region | Content |
|---|---|
| Top toolbar | Universe, range, timeframe, capital, costs, and Strategy from the Run Configuration; **Run** starts the default single active Backtest. |
| Center | Equity curve (`QLineSeries`) and optional presentation-only buy-and-hold overlay; benchmark alpha/beta remains deferred. |
| Right side panel | Summary metrics: total return, CAGR, Sharpe, max drawdown, win rate, # trades, exposure. |
| Bottom | Sortable trade log table (`QTableView` + `QSortFilterProxyModel`). |

**Cross-link.** A dedicated CTA (toolbar or footer button) SHOULD deep-link results into Replay so users can reconcile fills on the candlestick chart (`11` §1).

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
- **Volume pane** — histogram under the candles, sharing the categorical axis window (`11` §1).

### 2.4 Screener tab

The universe picker and predicate builder reuse accepted strategy-authoring
concepts where their semantics match (`11` §4):

| Region | Content |
| --- | --- |
| Top | Technical and fundamental fields only when verified Release Snapshot metadata exists; Python and AI remain planned. |
| Builder | Reuses typed Selectable Conditions where semantics match. Any future Python or AI path must follow Specs 05 and 12. |
| Logic | **Match ALL** vs **Match ANY** selector for typed built-in rows (maps to canonical `all` / `any`). |
| Actions | **Run Screener**, export (CSV/clipboard). |
| Results | `QTableView` with rank, symbol, company, price, change %, market cap, sector — plus optional performance/sector summary views in later milestones. |

### 2.5 Plugins tab

No Plugins tab is shipped in V1. If the future native-plugin system in Spec 08
is implemented, the UI begins with explicit file selection, hash display, trust
confirmation, ABI validation, and negotiated-capability display. It must not
scan and execute arbitrary libraries automatically, advertise plugin Strategy
execution, or invent a manifest interface before the ABI exists.

### 2.6 Logs tab

The planned Logs tab presents bounded structured application and Strategy logs
from the application data directory. Its exact file/schema contract remains
unimplemented and must not be treated as a current public interface.

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

The implemented
[`IChartView`](../../Src/Frontend/Include/Bte/Frontend/IChartView.h) seam has
three operations: replace the visible bar window, append one bar, and clear
markers. `QtChartsCandlestickView` is its current adapter.

Indicator overlays, persisted fill/corporate-action markers, crosshair state,
and result seeking are planned interface additions. Add them only with their
real callers, immutable value types, and tests; K-line Replay consumes persisted
result values rather than asking the chart to execute engine logic.

---

## 5. Persistence

| What | Location contract | Format/status |
|---|---|---|
| Strategies | OS-appropriate application data directory | Versioned typed condition artifacts or Python source plus versioned metadata; exact schema is owned by `05` |
| Screener presets | OS-appropriate application data directory | Planned typed universe/predicate artifacts; Python/runtime references only if that mode is promoted |
| Backtest Results | OS-appropriate application data directory | Target `.bteresult`; current legacy summaries remain JSON until migrated (`07`) |
| Settings and layout | OS-appropriate application data directory | Planned UI-only state; never a mutable market-data path |

One future Core path resolver owns these locations and must be redirectable in
tests. No public resolver name or subdirectory layout is accepted until its
implementation exists.

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
- Every action has a keyboard shortcut. Replay tab shortcuts: `Space` play/pause, `→` step, `←` step back, `1`/`5`/`9`/`0` set speed.
- Color choices for buy/sell markers also use shape (▲/▼) so colorblind users still distinguish.

---

## 9. Tests

- `Tests/Unit/Frontend/` uses Qt Test:
  - Smoke test: open each tab, ensure no crashes.
  - Replay state machine: simulate signals, assert UI labels update.
  - Strategy editor: invalid conditions or Python surface validation errors.
    AI-candidate smoke: **Run Backtest** stays disabled until the user accepts a
    complete imported candidate into an editor and normal validation succeeds.
- Visual regression on charts is **out of scope** for now; we eyeball it.
