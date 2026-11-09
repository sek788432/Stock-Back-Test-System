# Frontend (Qt)

> **Status:** Partially implemented. The current application contains the
> Strategies editor and first-milestone Python authoring workflow, the limited
> starter and Selectable Conditions Backtest workflows, Python Validate/Run/
> Cancel/Verify controls, Results management, and Replay. Python execution is
> implemented behind the managed-runtime contract. The optional macOS Apple
> Silicon build bundles the pinned profile; the default developer build omits
> it. Complete Results management, broader engine workflows, signed installer
> integration, manual release acceptance, and release-quality UX remain
> planned. Completed runs can be
> persisted and handed directly to Replay.

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

The first Python milestone is a focused editor and one recoverable draft, not a
saved Strategy library. It contains a Python source editor with the SMA sample,
fast/slow period and quantity controls (defaults 5/20/1), **Load `.py`**,
**Export `.py`**, **Validate**, and **Use in Backtest**. Draft source and
parameters are atomically autosaved; one prior valid generation supports
restart recovery. Load performs strict UTF-8 and size checks before replacing
the buffer. A dirty-buffer replacement presents **Save Draft**, **Discard**,
and **Cancel**. Validation displays stable error codes and source line/column
as selectable plain text.

**Use in Backtest** copies the current draft and parameters into Backtest setup
and switches pages; it never asks for consent or starts execution. At Run or
Verify time, the app freezes source, API/template versions, parameters, and the
selected immutable data/configuration identity, then requests explicit consent
for that source hash if needed. Source edits after capture remain a new draft
and cannot change the active execution.

Selectable Conditions remains available in Backtest with its existing form
contract; a complete reusable Strategy library, version history, import, Trash,
and restore are deferred. Nested groups, portfolio gates, generated Python,
and **Edit as Python** remain outside the first milestone.

Replay is entered from a stored Result and never starts a worker.

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

The Strategies page exposes **Validate** and **Cancel** for factory validation;
Backtest exposes Python **Run**, **Verify Reproducibility**, and a shared
**Cancel** action. These UI paths, source-hash consent, draft handoff, and
coordinator progress are implemented. The default developer app omits the
pinned managed runtime and reports a structured runtime-unavailable diagnostic.
Configure the macOS Apple Silicon app with
`BTE_BUNDLE_MANAGED_PYTHON_RUNTIME=ON` to assemble and embed the verified
profile; the optional bundle test starts a worker with that real profile. Signed
installer integration and manual end-to-end release acceptance remain planned.

Validate checks static constraints and the factory in a fresh managed worker
after hash-scoped consent; it creates no Result. Run freezes and executes one
source/parameter snapshot. The run panel shows bounded structured logs,
traceback with source line, terminal error code, fills, and Save Result / Save
& Replay / Discard actions. A normal run has no verification evidence and is
not shown with a verified label. The label appears only when matching three-run
evidence is available. If reading that evidence fails after a completed run,
the Result remains available and run diagnostics show the verification-status
warning.

Verify Reproducibility launches three fresh workers sequentially from one
frozen functional-input identity and shows attempt progress 1/3 through 3/3.
Cancel, mismatch, failure, and interruption remain distinct and never produce
a verified label. Only three completed attempts with identical canonical
hashes and inputs do. The verification record references each Result ID and
status/hash, stays separate from Result canonical records, and does not
auto-publish attempts. The user may continue editing the draft while a run is
active; edits to the current draft cannot change the frozen input used by
that run.

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

Opening, failing to open, or clearing a Result keeps playback isolated from the
bar-only preview: step, back, play, and preview-capital changes cannot display
stale preview candles or placeholder accounting. Loading preview data explicitly
returns to preview mode. Each time Replay becomes visible, it refreshes the saved
Result selector without resetting the current result or cursor. If a Result open
is pending, that catalog refresh waits for completion or an explicit return to
preview mode.

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

The current **View Details** pane lists the persisted capabilities for schema-2
and schema-3 Results as Supported or Unavailable. It also shows persisted
warning records as selectable plain text; when no warning was recorded it says
so explicitly. Unavailable families remain unavailable and are not represented
by synthesized values.

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
- **Python execution target:** an owned Bindings session carries immutable
  source/configuration values into Engine and owns the associated
  `PythonWorkerStrategy`. A generation ID travels with Validate, Run, Cancel,
  and Verify completions; a view applies only values for its live request and
  current widget lifetime. Destroying or switching a page cannot deliver a
  stale completion to a replacement draft. Cancel reaches the active parent
  process even while the worker is blocked in user code.
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
prices. Both use ordered actual-bar slots and one bottom UTC-label row in a
visually continuous, vertically stacked chart surface; the plot reserves
approximately 80–85% of its
height for price and 15–20% for volume. The default sliding viewport targets
roughly 24 pixels per selected bar slot and a 16–20-pixel candle body; zoom
adjusts density and body width.
Price auto-range uses only the high and low of candles in the visible viewport;
reached candles outside the viewport do not affect its scale. Volume auto-range
uses only visible volume and includes zero. Flat prices, high-volume extremes,
sparse prefixes, and zero-volume series retain finite, readable scales. Trade
markers may reserve visual padding beyond the candle range, but they do not
change the price data range.
`ResultReplay` owns the validated immutable timeline and replay cursor.
`ReplayTab` coordinates playback and viewport controls; the chart widget owns
`ReplayBarViewport`, the visual transform, slot mapping, autoscaling, and
drawing. One slot mapping drives candles, volume, markers, and UTC axes so
their coordinates cannot drift. Empty timelines make viewport operations
no-ops; wheel zoom is bounded, and pan, zoom, and resize remain clamped to
reached bars. When a candle has been reached, the viewport keeps at least one
candle visible. Result and data failures retain their structured errors rather
than being presented as fabricated or apparently successful replay data.
Price and volume use automatic plot layout with shared left insets so rendered
grids and series agree. Geometry updates are queued to avoid reentering Qt's
axis-range notifications. Insets retain their largest intrinsic label width,
not the already compensated plot position, so changing labels cannot accumulate
rounded blank margins during playback. This reservation is retained within a
timeline to avoid visible-boundary feedback, and reset on timeline replacement
or timeframe change. Volume batches replace hidden-series points once; newly
reached trade rows emit one insertion and one bounded cell-update notification.
When zoom temporarily hides the newest reached candle, Follow latest can
recenter immediately even before another replay frame arrives. Horizontal pan
enters Review history without changing the replay cursor, and playback does
not move the reviewed range. Wheel zoom anchors near the pointer;
button zoom anchors at the viewport center. Reset restores default density and
Follow latest. Fit reached bars is available only when each candle remains
legible. Timeframe changes rebuild the displayed series and reset viewport
state; resizing preserves the visible anchor as far as the new width allows.
Viewport navigation never changes the immutable result timeline or reveals
bars beyond the replay cursor. The accepted release adapter is project-owned
`QPainter`; it must not leak rendering types into the backend or alter
persisted ordering.

Indicator/corporate-action overlays, accessible marker-detail navigation, and
the release chart remain planned interface additions. K-line Replay consumes
persisted result values rather than asking the chart to execute engine logic.

---

## 5. Persistence

| What | Location contract | Format/status |
|---|---|---|
| Strategies | `QStandardPaths::AppLocalDataLocation/Strategies/PythonDraft` | One recoverable `python-draft.json` plus its previous recovery copy; users can import and export source as `.py`. A saved Strategy library is not implemented. |
| Backtest Results | `QStandardPaths::AppLocalDataLocation/Results/{Staging,Drafts,Results,Trash,Quarantine,Leases}` | Schema-2 and schema-3 `.bteresult` artifacts use opaque 32-digit lowercase hexadecimal Result IDs. The limited management UI supports the implemented catalog actions; the complete page remains planned. |
| Data and runtimes | Immutable release snapshots are under `QStandardPaths::AppLocalDataLocation/Data`; the managed runtime is bundled under app `Resources/RuntimeProfiles/{profileId}`. | Referenced immutable data segments and the optional pinned macOS runtime bundle are implemented. Runtime profiles are not stored in a user-writable app-data directory. |
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
frames. The first Python Strategy milestone also covers draft editing,
import/export, validation and consent flows, plus a managed-runtime GUI
backtest that saves and opens the ordinary result in Replay when the runtime is
available. These tests do not prove saved Strategy/complete Result library
pages, the full five-row editor, or the project-owned release chart.

The remaining future behaviors require positive, negative, and boundary tests
for one/five/six condition rows, invalid conditions, responsive layout, inner
scroll threshold, keyboard reorder, complete Result library flows, hash/schema
failures, Trash/Restore, atomic recovery, and **Open Replay** eligibility. The
release chart also requires deterministic render-geometry, interaction, and
accessibility tests; visual inspection alone is not evidence.
