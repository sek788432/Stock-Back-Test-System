# Backtest and Replay Product

This specification owns user-visible strategy storage, backtest setup, result
storage, and K-line Replay. Authoring details live in
[`StrategyAuthoring.md`](StrategyAuthoring.md); execution and persistence live
in [`EngineReplayPnL.md`](EngineReplayPnL.md).

## Honest delivery status

| Capability | Status | User-visible truth |
|---|---|---|
| K-line bar playback | **Implemented with known gaps** | Tracked hourly CSV bars and daily UTC aggregation support candlesticks, volume, persisted fill markers, play, pause, step, seek, and speed. Other timeframe choices remain planned. |
| Persisted replay portfolio strip | **Implemented limited slice** | It presents stored post-slice cash, position, market value, equity, and P&L for the implemented single-symbol Backtest path. |
| Legacy JSON replay summaries | **Removed** | No runtime or test path remains; transactional SQLite `.bteresult` storage is implemented. |
| Starter Backtest page | **Implemented limited slice** | One symbol/range, fixed starter or limited Selectable Conditions, whole-share long-only market behavior, and next-actual-bar execution. |
| Saved strategies and complete Results library | **Accepted design / partially implemented** | Backend Result storage, catalog, import, and lifecycle operations exist, but saved Strategies and the complete Results management page remain planned. |
| Durable results and result replay | **Implemented limited slice** | The current Backtest records canonical warnings, orders, fills, fixed-point slippage costs, and one post-slice portfolio checkpoint per processed bar; promotes completed or diagnostic `.bteresult` artifacts; and can open them in Replay without engine execution. Unsupported record families are declared in persisted capability metadata. The complete engine, canonical record families, and result migration remain planned. |
| Python Strategy and Debug Run | **Accepted design / not implemented** | No shipped Python worker or Debug Run path exists yet. |
| Public data-bearing release | **Blocked** | Redistribution rights and verified split metadata are missing. |

The UI, README, release notes, and screenshots must preserve these distinctions.

## Application navigation

The accepted product has separate **Strategies**, **Backtest**, **Results**, and
**Replay** pages. The current shell exposes exactly those four pages;
Strategies and Results remain placeholders until their accepted libraries are
implemented. AI authoring and a canonical Stock Screener remain outside
accepted scope.

- **Strategies** stores reusable authoring artifacts. A saved strategy can fill
  the strategy section of the Backtest page but never starts a run by itself.
- **Backtest** edits a Run Configuration and executes it.
- **Results** stores completed and diagnostic Backtest Results for inspection,
  export, comparison, and opening in Replay.
- **Replay** presents one existing result and its exact retained data. It does
  not accept a Strategy and does not execute the engine.

## Strategy library

This section is the accepted target; saved Strategy persistence and its UI are
not implemented in the current checkout.

Strategy artifacts are stored in `Strategies/Active` using UUIDv7 filenames
and the `.btestrategy` extension. The library shows name, authoring mode,
API/schema version, modified time, validation state, Runtime Profile, and
last-used time. It sorts by most recently modified by default.

Actions are New, Edit, Duplicate, Import, Export, Validate, Use in Backtest,
Version History, Move to Trash, and Restore. Saving an edit creates a new
immutable version under the same logical Strategy ID. Older versions remain in
Version History until explicitly trashed; existing Results retain the exact
version and hash they used.

Names are required trimmed Unicode text from 1 through 100 characters.
Duplicates are allowed because UUID identity is authoritative; the UI
disambiguates with mode and modified time. Import previews and validates an
untrusted artifact's schema, size, hashes, mode/API, and identifiers before
staging and atomic promotion. Import never executes source. Python still
requires consent before first execution and whenever its source hash changes.

**Use in Backtest** performs normal validation, then selects that exact Strategy
version and switches pages without starting a run. If Backtest contains unsaved
edits, the user must choose Save, Discard, or Cancel before replacement. A run
still requires an explicit **Run** action and never mutates the saved artifact.

## Backtest setup

A Run Configuration selects an immutable snapshot, ordered symbols, timeframe,
half-open UTC range, strictly positive initial capital, costs, a finite annual
risk-free rate greater than `-1`, numeric profile, and one exact Strategy
version. Invalid values are rejected before execution. The default supports one
active Backtest.

Selectable Conditions offer separate buy and sell groups, each with one to
five rows and flat **ALL** or **ANY** logic. The V1 editor exposes only the
current condition fields, comparisons, whole-share sizing, and long-entry/
close-long actions. Nested groups, portfolio gates, generated Python, and
**Edit as Python** are outside scope.

At widths of at least 1100 px, buy and sell groups appear side by side; at
narrower widths they stack. Each group has a compact sticky header and Add
control. When its content exceeds the larger of 420 px or 40% of the viewport,
that group scrolls vertically inside its panel. Horizontal scrolling is not
allowed. Rows expose clear validation and keyboard-accessible reordering.

## Result library and storage

The implemented limited backend stores staged, promoted, and trashed
`.bteresult` artifacts below the application Results root. Each filename uses
the run's opaque 32-digit lowercase hexadecimal Result ID. The backend supports
begin/append/finalize/promote, validated catalog listing/open, import,
interrupted-run recovery, Trash/restore, and purge for schema 2. The current
Results tab is still a placeholder; Replay owns the implemented catalog
selector and Backtest can hand it an exact promoted Result ID.

The accepted complete Results page shows status, Strategy name,
symbols/universe, timeframe, range, completion time, valid total return,
result-schema version, canonical-hash state, and data availability. It sorts
newest first by default; selecting a row opens a read-only detail pane.

Actions are Show Details, Open Replay, Compare, Export, Import, Move to Trash,
and Restore. Filters cover Strategy, symbol/universe, timeframe, status, and
date range, with separate Active and 30-day Trash views.
**Open Replay** is disabled when validation or required data resolution fails
and displays the exact structured error.

Implemented Result writes use same-filesystem staging, validation, close, and
no-clobber promotion. Backend import treats the artifact as untrusted data and
validates its current schema, canonical hash, identifiers, and data references;
it never executes embedded source. Result Trash/restore/purge exists in the
backend, while its complete UI and 30-day scheduler remain planned. Strategy
artifact persistence remains wholly planned and must not be inferred from the
Result implementation.

## K-line Replay

Implemented limited Result Replay opens a validated `.bteresult` plus its exact
persisted row spans from retained Data Segments; it never reselects by the
broader Run date range. An owned Bindings request object performs cancellable
catalog/open work and publishes only the newest generation as a queued immutable
value. Replay shows Hourly or UTC Daily candles, synchronized
volume, persisted fills/markers, post-slice cash/position/market-value/equity/P&L,
partial UTC-day state, and terminal reason. It uses indexed frames, displays at
most 500 at once, and never reruns Python, indicators, order evaluation, or
fills. Complete warnings/logs/metrics/trade episodes and marker-detail
interaction remain planned.

The current development chart is Qt Charts with a volume line and secondary
axis. The accepted project-owned `QPainter` release chart uses a 70/30
candlestick-to-volume vertical split, initially presents 120 bars, and caps its
visible window at exactly 1,000 bars. Its pan, zoom, seek, crosshair, accessible
marker navigation, and keyboard playback must preserve persisted ordering and
never fabricate data.

## Required verification

- Positive, negative, and boundary tests cover each public view-model action,
  including one/five/six condition rows, responsive layout thresholds, inner
  scrolling, validation, keyboard reorder, import, Trash, and atomic recovery.
- Integration tests cover strategy-to-Backtest fill-in, exact strategy-version
  capture, result reopening without execution, corrupt/untrusted imports,
  missing data, and schema-version edges.
- Replay tests prove that opening, seeking, and charting a result cannot invoke
  Strategy hooks, Python, indicator scheduling, order evaluation, or fills.
- Accessibility tests cover focus order, labels, keyboard-only operation, and
  status/error announcements.
