# Backtest and Replay Product

This specification owns user-visible strategy storage, backtest setup, result
storage, and K-line Replay. Authoring details live in
[`StrategyAuthoring.md`](StrategyAuthoring.md); execution and persistence live
in [`EngineReplayPnL.md`](EngineReplayPnL.md).

## Honest delivery status

| Capability | Status | User-visible truth |
|---|---|---|
| K-line bar playback | **Implemented with known gaps** | Tracked hourly CSV bars and daily aggregation support candlesticks, volume, play, pause, step, and speed. Other displayed timeframe choices are unsupported. |
| Current replay portfolio strip | **Implemented placeholder** | It shows initial values, not trade-driven accounting. |
| Legacy JSON replay summaries | **Removed** | No runtime or test path remains; durable `.bteresult` storage is still not implemented. |
| Starter Backtest page | **Implemented limited slice** | One symbol/range, fixed starter or limited Selectable Conditions, whole-share long-only market behavior, and next-actual-bar execution. |
| Saved strategies and results | **Accepted design / not implemented** | Separate Strategy and Result libraries follow the contracts below. |
| Python Strategy, Debug Run, durable results, and result replay | **Accepted design / not implemented** | No shipped worker or `.bteresult` path exists yet. |
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

Results are stored in `Results/Active` using UUIDv7 filenames and the
`.bteresult` extension. The table shows status, Strategy name, symbols/universe,
timeframe, range, completion time, valid total return, result-schema version,
canonical-hash state, and data availability. It sorts newest first by default;
selecting a row opens a read-only detail pane.

Actions are Show Details, Open Replay, Compare, Export, Import, Move to Trash,
and Restore. Filters cover Strategy, symbol/universe, timeframe, status, and
date range, with separate Active and 30-day Trash views.
**Open Replay** is disabled when validation or required data resolution fails
and displays the exact structured error.

Both artifact types are written to a same-filesystem staging path, fully
validated, flushed, and atomically renamed into `Active`. Import is untrusted:
it validates size, schema versions, hashes, identifiers, and references and
never executes embedded strategy source. Deletion moves an artifact into its
type-specific Trash; eligible unreferenced content is purged after 30 days.

Before persistence exists, both library pages show honest empty states rather
than fake content. Results state that no `.bteresult` files exist and result
persistence is planned; Strategies state that saved Strategy persistence is
planned.

## K-line Replay

Replay opens a validated `.bteresult` plus its exact retained Data Segments. It
shows persisted candlesticks, volume, fills, warnings, portfolio/accounting
records, metrics, and structured strategy logs; it never reruns Python,
indicators, order evaluation, or fills.

The project-owned `QPainter` chart uses a 70/30 candlestick-to-volume vertical
split. It initially loads 120 bars and supports an exact maximum of 1,000
visible bars. Pan, zoom, seek, crosshair, accessible marker navigation, and
keyboard playback must preserve persisted ordering and never fabricate data.

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
