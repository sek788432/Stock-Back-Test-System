# Frontend (Qt)

> **Status:** Partially implemented. The current application contains Qt tab,
> chart, replay scaffolding, and a Backtest page for the limited engine slice
> documented in [`EngineReplayPnL.md`](EngineReplayPnL.md) §1. That page supports the fixed starter strategy and the
> implemented Selectable Conditions subset; saved-strategy authoring, Python
> strategies, complete backtest workflows, the complete Results library, and
> release-quality UX remain planned unless their owning spec says otherwise.
> The current shell exposes Strategies, Backtest, Results, and Replay;
> Strategies is a placeholder; Results has a limited management page with
> catalog, details, Open Replay, Move to Trash and Restore. The complete
> Results library remains planned.
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
The current development view uses separate synchronized price and volume panes,
with one OHLC candle and one colored volume bar per selected UTC Hourly bar or
Daily bucket. Replay progressively presents every selected bar without a fixed
frame cap; the release replacement must preserve this behavior and land with
functional, accessibility, and performance tests before distribution.

---

## 2. Window structure

The current **Help → Build Information** dialog displays launcher-provided
checkout path, branch, commit, dirty state, built executable SHA-256 and
selected immutable snapshot ID. Direct execution without launcher provenance
shows `unavailable` for missing fields. This is a diagnostic identity display,
not a verification of market-data quality or a substitute for testing the
actual launched workflow.

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

Each Selectable Conditions metric updates its threshold suffix, tooltip, and
accessible description immediately to explain USD price or difference,
percent-point change, 0–100 oscillator level, or shares, with a concrete
example. This is a display-only mapping; it neither rescales the threshold nor
changes the condition's numeric domain. Period tooltips say actual bars
and explain warm-up. Metrics whose formula does not use a period say so;
their period remains visible and editable because its existing typed value
participates in strategy identity. This display-only slice does not normalize
that value or change identity semantics.

| Region | Content |
|---|---|
| Top toolbar | Universe, range, timeframe, capital, costs, and Strategy from the Run Configuration; **Run** starts the default single active Backtest. |
| Center | Equity curve (`QLineSeries`) and optional presentation-only buy-and-hold overlay; benchmark alpha/beta remains deferred. |
| Right side panel | Summary metrics: total return, CAGR, Sharpe, max drawdown, win rate, # trades, exposure. |
| Bottom | Sortable trade log table (`QTableView` + `QSortFilterProxyModel`). |

**Cross-link.** A completed or diagnostic stored Result has a dedicated **Open
Replay** action.

### 2.3 Replay tab — implemented modes and deferred UX

The current tab keeps an explicitly labelled **Bar-only preview** keyed by
symbol, timeframe/range, and placeholder initial capital. It has no Strategy,
fills, or authoritative accounting. Hourly CSV and UTC-daily aggregation are
its only supported paths; unsupported timeframe choices are not presented, and
a preview load failure still becomes an empty collection.

The implemented limited **Result Replay** mode asynchronously lists and opens
validated `.bteresult` artifacts and retained Data Segments. It supports Hourly
and UTC Daily presentation, immediate first-frame display, cancellation and
stale-request rejection, step/back/seek/playback, an untruncated progressive
candle prefix, volume, synchronized persisted fill markers/rows, post-slice
portfolio values, partial-day labelling, and terminal reasons. Hourly can replay
every saved hourly candle and UTC Daily every aggregate daily candle over the
same saved range. Changing timeframe preserves the current UTC date when
that date exists in the destination timeline. Replay setup is collapsed by
default behind a keyboard-focusable **Show replay setup** toggle. The chart has
a 500-pixel minimum height, occupies at least 55% of the Replay viewport at the
tested 1024×768, 1200×800, and 1440×900 sizes, and shares a vertical splitter
with the portfolio/trade details so users can adjust their relative height.
The 1024×768 collapsed state does not require horizontal scrolling. It does not
invoke Strategy, Python, indicator scheduling, order evaluation, or Engine
execution. Complete warning/log/metric presentation and marker detail
interaction remain planned. A limited Results-management page is implemented.

The headline UX. Layout:

```
┌─────────────────────────────────────────────────────────────────────┐
│  ▸ Show replay setup                                                │
│  ◀ Back  ▶ Play  Step   Speed [1× 5× 10× max]  ███████░░░  Out In  │
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
             ↕ draggable chart/details splitter
```

Interactions:
- **Speed control** — 1× presents one persisted slice per second; max presents
  records without intentional delay. It does not pace engine execution.
- **Pause / Step** — `Space` pauses/resumes presentation; `→` presents the next
  persisted slice. Presentation starts with one candle; playback, step, speed,
  seek, and back grow or shrink the visible `0..cursor` candle prefix together
  with its portfolio state. Reached candles are not discarded after 500 frames.
- **Scrub bar** — seek within validated persisted checkpoints/records and
  referenced bars; seeking never reconstructs fills by rerunning the engine.
  Fill markers and synchronized trade rows appear with the persisted fill bar;
  rewinding hides both future candles and their fills.
- **Trade markers** — persisted buys use green upward triangles and sells use
  red downward triangles. A Buy glyph is placed below the containing candle's
  low; a Sell glyph is placed above its high. Both share that candle's x-position,
  so the visual mark remains adjacent to the correct K bar. The 16-pixel glyph
  bounding box leaves sixteen logical pixels of clearance from the wick; its
  triangle tip is slightly farther away. A fixed 33-pixel display reserve on
  each price edge prevents clipping at supported Replay sizes. The pixel-derived
  padding is capped at half the visible price span for very short plots; the
  existing one-USD display-padding floor still applies to flat/near-flat prices.
  Plots under 132 logical pixels may not fit the full reserve. Height-only
  resize recalculates this reserve. Price ticks may extend below zero as visual
  whitespace for a positive low near zero; the stored OHLC and financial values
  are never changed.
  Buy fills sharing a displayed candle use one triangle without a leader. Its
  tooltip retains the first reached fill's details and, for multiple Buys,
  states the reached fill count and points to the complete trade log. Group by
  displayed candle, not identical fill time: Hourly uses its source bar and
  Daily uses its UTC-date candle. Coincident Sell fills remain deterministically
  separated with leader lines; a single Sell has no leader. Every fill's exact
  timestamp and price remain unchanged in persisted results and synchronized
  per-fill trade rows. The chart retains one price point per fill at its
  containing candle's x slot, not an exact-time x coordinate. Rewind/replacement resets groups and their counts; future
  fills cannot contribute to a displayed tooltip. Daily glyphs anchor to their containing UTC candle
  while retaining the actual fill time. Click-to-open fill details remains
  planned.
- **Cursor inspector** — the current Qt Charts adapter has a crosshair; an
  OHLCV/indicator side popup remains planned.
- **Volume** — the current adapter draws one direction-colored histogram bar
  per selected K bar in a separate lower pane. Its UTC time axis shares the
  upper price pane's range and horizontal plot geometry; its y axis shows
  whole shares independently of quote-price ticks. The planned project-owned
  release chart may replace the adapter without changing the saved OHLCV.

### 2.4 Results tab

The current Results tab has a limited asynchronous saved/Trash catalog,
read-only details, same-ID Open Replay, Rename, confirmed Move to Trash and
Restore. A saved Result's optional display name is shown first in the catalog
and Replay selector; the immutable ID remains visible and is still used for
all navigation and storage operations. The Backtest save actions offer an
optional name field, and legacy unnamed artifacts display their ID.
It does not yet implement the accepted complete page. That page is separate
from Strategies. Its proxy-model table shows the
stored result's Strategy, universe, timeframe, range, status, completion time,
valid total return, result-schema version, canonical-hash state, and data
availability. Actions are Show Details, Open Replay, Compare, Export, Import,
Move to Trash, and Restore. Filters cover Strategy, universe/symbol, timeframe,
status, and date, with separate Active and 30-day Trash views.

Imports are untrusted data. The UI validates them without executing embedded
source. **Open Replay** remains disabled with a structured reason when the
result or referenced data is invalid or unavailable.

The current **View Details** pane lists every schema-2 Result capability as
Supported or Unavailable using the persisted capability metadata. It also
shows persisted warning records as selectable plain text; when no warning was
recorded it says so explicitly. Unavailable families remain unavailable and
are not represented by synthesized values.

---

## 3. View-model seam

Catalog requests carry value-owned entries and typed recovery notices from
one opened Results store. Results, Replay, and Backtest's Unsaved Runs show
English preservation warnings beside usable entries; Replay keeps these
warnings outside its collapsed setup. The warning text is plain, selectable
text and does not offer deletion, quarantine, or automatic repair. A subsequent
successful catalog refresh replaces the notices, including hiding an empty
notice set. Catalog initialization failures remain distinct from a successful
catalog containing preserved-artifact notices. The notice label exposes the
same current English text through its accessible name and is hidden when a
successful refresh returns no notices.

- **Implemented baseline:**
  [`ReplaySessionVm`](../../Src/Bindings/Include/Bte/Bindings/ReplaySessionVm.h)
  is a synchronous, dependency-light adapter over the current bar-only Replay.
  It owns the backend with `std::unique_ptr`; its portfolio values are explicitly
  placeholders, not engine accounting.
- **Implemented result adapter:**
  [`ResultReplay`](../../Src/Bindings/Include/Bte/Bindings/ResultReplay.h)
  validates persisted results/data and exposes indexed immutable presentation
  frames. Its complete immutable timeline is the chart context; its bounded
  cursor history is not a chart-window contract. `ReplayTab` owns independent
  catalog and open
  [`ResultReplayRequests`](../../Src/Bindings/Include/Bte/Bindings/ResultReplayRequests.h)
  channels; those Bindings objects own concurrency, cancellation,
  per-channel stale-generation rejection, and queued immutable completion.
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
[`IChartView`](../../Src/Frontend/Include/Bte/Frontend/IChartView.h) seam can
replace or append bars and set or clear persisted fill markers.
`QtChartsCandlestickView` renders price candles over aligned per-bar volume,
persisted Buy/Sell markers, crosshair, pan, and zoom. Its Hourly time labels
include date and hour, Daily labels include date, and the time axis declares
UTC. Price and volume use separate y axes, so shares cannot rescale quote
prices. The default sliding viewport targets roughly 24 pixels per selected
bar slot and a 16–20-pixel candle body; zoom adjusts density and body width.
Price and volume use automatic plot layout with shared left insets so rendered
grids and series agree. Geometry updates are queued to avoid reentering Qt's
axis-range notifications. Insets retain their largest intrinsic label width,
not the already compensated plot position, so changing labels cannot accumulate
rounded blank margins during playback. This reservation is retained within a
timeline to avoid visible-boundary feedback, and reset on timeline replacement
or timeframe change. Volume batches replace hidden-series points once; newly
reached trade rows emit one insertion and one bounded cell-update notification.
When zoom temporarily hides the newest reached candle, Follow latest can
recenter immediately even before another replay frame arrives. The accepted
release adapter is project-owned
`QPainter`; it must not leak rendering types into the backend or alter
persisted ordering.

Indicator/corporate-action overlays, accessible marker-detail navigation, and
the release chart remain planned interface additions. K-line Replay consumes
persisted result values rather than asking the chart to execute engine logic.

---

## 5. Persistence

| What | Location contract | Format/status |
|---|---|---|
| Strategies | `QStandardPaths::AppLocalDataLocation/Strategies/{Active,Trash}` | UUIDv7 `.btestrategy` typed plan or Python source plus versioned metadata |
| Backtest Results | `QStandardPaths::AppLocalDataLocation/Results/{Staging,Drafts,Results,Trash,Quarantine}` | Implemented limited schema-2 `.bteresult` artifacts named by opaque 32-digit lowercase hexadecimal Result IDs; limited management UI exists, complete management remains planned |
| Data and runtimes | `QStandardPaths::AppLocalDataLocation/{DataSegments,RuntimeProfiles}` | Limited immutable referenced segments are implemented; complete release profiles remain planned |
| Settings and layout | OS-appropriate application data directory | Planned UI-only state; never a mutable market-data path |

One future Core path resolver owns these locations and is redirectable in tests.
Writers stage, validate, flush, and promote artifacts without clobbering on
the same filesystem.

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
page, bar-only and result-backed Replay states, asynchronous switching and
destruction, the Qt Charts candle/volume/marker adapter, and Bindings result
frames. They do not prove saved Strategy/complete Result library pages, the
five-row editor, Python authoring, or the project-owned release chart.

Those accepted future behaviors require positive, negative, and boundary tests
for one/five/six condition rows, invalid conditions, responsive layout, inner
scroll threshold, keyboard reorder, Python validation, Result import,
hash/schema failures, Trash/Restore, atomic recovery, and **Open Replay**
eligibility. The release chart also requires deterministic render-geometry,
interaction, and accessibility tests; visual inspection alone is not evidence.
