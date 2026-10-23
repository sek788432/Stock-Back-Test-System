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
| Task 17 previous data-day close condition | **Implemented limited slice** | Backtest offers positive-magnitude Down/Up thresholds against the previous available UTC data day's last CSV source close, including extended hours; history comes from the same immutable snapshot and stays out of Result/Replay bars. |
| Task 18 historical data-day conditions | **Implemented limited slice** | Backtest offers a 30-data-day close-to-close gain and 21-data-day high/low drawdown, rebound, and strict breakout conditions. Prior UTC data dates come from the same immutable snapshot and stay out of Result/Replay bars. |
| Saved strategies and complete Results library | **Accepted design / partially implemented** | Backend Result storage and a limited Results management page exist, but saved Strategies and the complete Results library remain planned. |
| Durable results and result replay | **Implemented limited slice** | The current Backtest records canonical warnings, orders, fills, fixed-point slippage costs, and one post-slice portfolio checkpoint per processed bar in a durable draft. Save publishes the same `.bteresult` artifact, which Replay can open without engine execution. Unsupported record families are declared in persisted capability metadata. The complete engine, canonical record families, and result migration remain planned. |
| Python Strategy and Debug Run | **Accepted design / not implemented** | No shipped Python worker or Debug Run path exists yet. |
| Public data-bearing release | **Blocked** | Redistribution rights and verified split metadata are missing. |

The UI, README, release notes, and screenshots must preserve these distinctions.

## Application navigation

The accepted product has separate **Strategies**, **Backtest**, **Results**, and
**Replay** pages. The current shell exposes exactly those four pages;
Strategies remains a placeholder. Results now has limited list/details,
Open Replay, Trash and Restore actions; its full library remains planned.
AI authoring and a canonical Stock Screener remain outside
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

The current editor exposes at most two rows per group, including the Task 17
**Previous data-day close change %** metric and Task 18 historical conditions.
Task 17 compares against the last source close from the previous available UTC
date. Task 18 offers **Up X% over selected period**, **Down X% from period
high**, **Up X% from period low**, **Break period high**, and **Break period
low**. The gain defaults to 30 available UTC data dates; extrema and breakouts
default to 21 completed dates. Extended-hours source rows count, the entire
current UTC date is excluded, and missing or insufficient history does not
match. Percentage thresholds use positive magnitudes; breakout comparisons are
strict. Historical pre-range data comes from the same immutable snapshot and
does not add Result or Replay bars. See the
[`StrategyAuthoring.md`](StrategyAuthoring.md#csvutc-day-historical-conditions)
contract.

At widths of at least 1100 px, buy and sell groups appear side by side; at
narrower widths they stack. Each group has a compact sticky header and Add
control. When its content exceeds the larger of 420 px or 40% of the viewport,
that group scrolls vertically inside its panel. Horizontal scrolling is not
allowed. Rows expose clear validation and keyboard-accessible reordering.

## Result library and storage

### Implemented explicit-save flow (limited Backtest slice)

The maintainer approved durable recoverable execution drafts with explicit
publication. A run records trustworthy execution-time records before any Save
action. Its unsaved draft is not an active saved entry. **Save Result** validates
and publishes that same run to Results without leaving Backtest; **Save & Replay**
performs the same idempotent save and navigates using its exact Result ID.
Neither action starts another Engine execution or changes canonical records.
An optional Result name is entered beside the save actions. It is trimmed and
validated before publication; a nonempty name is stored as mutable display
metadata, never as the immutable Result ID or canonical hash input. Results
can rename a saved entry later without rerunning Engine. Empty/legacy names
fall back to the Result ID in catalog and Replay selection. A renamed result
retains its name through Trash and Restore, while exact-ID navigation remains
unchanged.
Unsaved drafts survive restart until explicit Save or Discard. Starting another
run preserves the prior draft in an Unsaved Runs list; exit neither publishes
nor discards it. A terminal draft remains outside the active Results catalog;
interrupted staging with trustworthy records recovers as a diagnostic draft.
The limited implementation does not establish complete mid-run durability or
close the separately tracked persistence and retention review findings.

The implemented limited backend stores staged, draft, promoted, and trashed
`.bteresult` artifacts below the application Results root. Each filename uses
the run's opaque 32-digit lowercase hexadecimal Result ID. The backend supports
begin/append/finalize-draft/explicit-publish, draft listing/discard, validated
catalog listing/open, import, interrupted-run recovery, Trash/restore, and
purge for schema 2. The limited Results tab lists Active/Trash entries, opens
validated details, navigates by exact Result ID, and offers confirmed Move to
Trash, Restore, and Rename. Replay also has its own named catalog selector,
and Backtest can
hand it an exact promoted Result ID.

The limited Result details view enumerates every schema-2 capability from the
persisted capability metadata and displays persisted warning records. It does
not fabricate values for unsupported trade, metric, log, indicator, corporate
action, or margin families.

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
backend; its limited UI exists, while the complete UI and 30-day scheduler
remain planned. Strategy
artifact persistence remains wholly planned and must not be inferred from the
Result implementation.

## K-line Replay

Implemented limited Result Replay opens a validated `.bteresult` plus its exact
persisted row spans from retained Data Segments; it never reselects by the
broader Run date range. An owned Bindings request object performs cancellable
catalog/open work and publishes only the newest generation as a queued immutable
value. Replay shows Hourly or UTC Daily candles, synchronized
volume, persisted fills/markers with selected source-bar indexes, post-slice
cash/position/market-value/equity/total P&L plus realized and unrealized P&L,
partial UTC-day state, and terminal reason. It uses indexed frames, progressively
displays every selected bar through the current cursor without a fixed frame cap,
and never reruns Python, indicators, order evaluation, or
fills. Complete warnings/logs/metrics/trade episodes and marker-detail
interaction remain planned.

The current development chart is Qt Charts with aligned price-candle and
colored volume-bar panes. Hourly ticks show UTC date/hour and Daily ticks show
UTC date; price and volume have separate y scales. The accepted project-owned
`QPainter` release chart must preserve the complete progressive selected-bar
timeline, synchronized panes, pan, zoom, seek, crosshair, accessible marker
navigation, and keyboard playback without fabricating data.

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
