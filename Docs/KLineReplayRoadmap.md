# K-Line Replay Roadmap

> **Status:** Historical planning aid, not a behavioral specification or implementation-status source. Use `Specs/07EngineReplayPnL.md` and `Specs/11StockScreenerKLineProduct.md` for current accepted behavior.

This document tracks the implementation plan for Zoe's K-line replay ownership area.
It is a planning map for GitHub issues, not a replacement for the product specs.

Authoritative specs:

- `Docs/Specs/02FrontendQt.md`
- `Docs/Specs/04DataLayer.md`
- `Docs/Specs/07EngineReplayPnL.md`
- `Docs/Specs/11StockScreenerKLineProduct.md`

Current repository state: CSV-backed data loading, bar-step replay, replay-clock
controls, Qt candlestick/volume presentation, and legacy JSON replay summaries
are implemented. Strategy execution, orders/fills, portfolio accounting, managed
Release Snapshots, and `.bteresult` remain planned. See the status table in
`Docs/Specs/11StockScreenerKLineProduct.md` rather than inferring delivery
status from the historical phases below.

## Product Contract

K-line replay is the single-symbol candlestick playback surface described in
`Docs/Specs/11StockScreenerKLineProduct.md` section 1. The feature must let
the user choose a symbol, timeframe/schema, date range, and initial capital, then
play historical OHLCV bars one bar at a time on a candlestick chart.

The minimum user-facing contract is:

- inputs: symbol, schema/timeframe, start date, end date, initial capital
- chart output: candlesticks plus a volume pane
- playback: step, play, pause, speed multiplier, and max-speed mode
- state output: current bar, bar index, total bars, replay state, and visible chart window
- later result output: portfolio strip, buy/sell markers, trade log, metrics, and saved Backtest Results

The Qt implementation follows `Docs/Specs/02FrontendQt.md`: Qt 6 Widgets,
Qt Charts for candles, an `IChartView` abstraction, and view-models that keep the
UI isolated from backend internals.

The backend implementation follows `Docs/Specs/04DataLayer.md` and
`Docs/Specs/07EngineReplayPnL.md`: data arrives through `BarStream`, replay is
paced by `ReplayClock`, and functional output must stay deterministic for the
same input bars and engine config.

## Phase 1 - K-Line Replay MVP

> **Current disposition:** Implemented baseline. The shipped behavior is the
> bar-only replay described by Specs 07 and 11; it is not a trading Backtest.

Goal: deliver a visible, testable replay loop that can read fixture/CSV bars, step or play through them, and render K-line candles in the Qt UI. Backend integration may use fake or fixture-backed components until production data and strategy execution are ready.

### Phase 1 Required Behavior

Frontend:

- Create a Replay tab under the Qt app shell (historical task wording; the
  baseline tab is now implemented).
- Show controls for symbol, schema/timeframe, start date, end date, initial capital, step, play, pause, and speed.
- Render candlesticks from fixture or CSV bars through `IChartView`.
- Keep the UI responsive while replay advances.

Backend:

- Read bars through a CSV-backed `BarStream`.
- Provide `Replay::step()` for deterministic one-bar advancement.
- Emit immutable replay progress snapshots for the UI.
- Support automatic play/pause and speed presets.

Playback speed contract:

| Mode | Meaning |
| --- | --- |
| `1x` | 1 bar per second |
| `5x` | 5 bars per second |
| `10x` | 10 bars per second |
| `max` | drain bars as fast as possible without intentional sleep |

Phase 1 is complete when a user can load fixture or CSV bars, step one candle at
a time, play continuously, change speed, pause safely, and see candles update on
the Replay tab.

| Issue | Type | Task | Outcome |
| --- | --- | --- | --- |
| [#4](https://github.com/sek788432/Stock-Back-Test-System/issues/4) | BE | CSV-backed `BarStream` | Replay can read OHLCV bars from small CSV fixtures. |
| [#5](https://github.com/sek788432/Stock-Back-Test-System/issues/5) | BE | Replay clock and single-step replay | `Replay::step()` advances exactly one bar at a time. |
| [#6](https://github.com/sek788432/Stock-Back-Test-System/issues/6) | BE | Replay progress snapshots | UI can consume immutable per-bar progress snapshots. |
| [#18](https://github.com/sek788432/Stock-Back-Test-System/issues/18) | BE | Play/pause/speed replay controls | Replay supports 1x, 5x, 10x, and max-speed playback. |
| [#7](https://github.com/sek788432/Stock-Back-Test-System/issues/7) | FE | K-line replay tab skeleton | Qt has a Replay tab with symbol, schema, date range, capital, playback, and speed controls. |
| [#8](https://github.com/sek788432/Stock-Back-Test-System/issues/8) | FE | Candlestick chart rendering | Qt Charts renders candles and basic volume from fixture bars. |
| [#9](https://github.com/sek788432/Stock-Back-Test-System/issues/9) | FE + BE | `ReplaySessionVm` wiring | Replay controls update the chart through a view-model without freezing the UI. |
| [#10](https://github.com/sek788432/Stock-Back-Test-System/issues/10) | Test | Deterministic replay fixture | Replaying the same fixture bars produces stable snapshots. |

Phase 1 demo target:

1. Open the Replay tab.
2. Load fixture or CSV bars.
3. Press Step to append one candle.
4. Press Play to advance repeatedly.
5. Switch speed between 1x, 5x, 10x, and max.
6. Pause without freezing the UI.

## Phase 2 - Production Data And Replay Results

> **Current disposition:** Superseded target. The
> [engine and release-data decision](Decisions/ImportantDecisions.md#engine-and-release-data-authority)
> replaced direct runtime DuckDB access and generic replay-result snapshots
> with immutable Release Snapshots and transactional `.bteresult` Backtest
> Results. The issue links are retained as historical context, not accepted
> implementation instructions.

Goal: connect replay to production-style data and persist completed Backtest
Results for later comparison. Durable Backtest Results are separate from
in-memory progress snapshots used by the UI.

### Phase 2 Required Behavior

Data:

- Build an immutable, versioned Release Snapshot from tracked
  `StockData/Extracted` inputs outside the application runtime.
- Discover symbols and supported timeframes from its validated manifest.
- Open bounded streams filtered by snapshot, universe, timeframe, and range.
- Keep DuckDB developer-only; the release application neither reads nor writes
  `StockData/MarketData.duckdb`.

Replay results:

- Persist each run as a transactional SQLite `.bteresult` Backtest Result.
- Treat the Backtest Result as a durable artifact distinct from transient UI
  progress snapshots.
- Persist canonical events, configuration, strategy/runtime identities, and
  exact content-addressed Data Segment references.

Portfolio and trades:

- Emit portfolio snapshots for cash, position, market value, equity, and PnL.
- Emit trade snapshots that the UI can display as chart markers and trade-log rows.

| Issue | Type | Task | Outcome |
| --- | --- | --- | --- |
| [#11](https://github.com/sek788432/Stock-Back-Test-System/issues/11) | BE | Historical read-only DuckDB `DataSource` | **Superseded:** replace with an immutable Release Snapshot reader. |
| [#19](https://github.com/sek788432/Stock-Back-Test-System/issues/19) | BE | Persist replay results | Re-scope to transactional `.bteresult` output and canonical hash validation. |
| [#12](https://github.com/sek788432/Stock-Back-Test-System/issues/12) | BE | Portfolio snapshots and trade markers | Replay can emit cash, position, equity, PnL, and buy/sell marker data. |
| [#13](https://github.com/sek788432/Stock-Back-Test-System/issues/13) | FE | Portfolio strip and trade log | Replay UI displays portfolio state, trade rows, and chart markers. |

Backtest Results include enough canonical identity to compare runs later:

- symbol
- schema name / timeframe
- start and end range
- initial capital
- strategy id/name
- strategy artifact hash
- engine config
- equity curve
- trade log when available
- metrics when available
- non-functional creation timestamp
- required Release Snapshot, Data Segment, calendar, and split-manifest hashes
- result-schema, engine, strategy API, runtime, and numeric-policy versions
- `canonicalResultHash`

Suggested persistence location:

- `<userData>/results/` for comparable run outputs, or
- `<userData>/sessions/` if implementation chooses to align with the existing
  session persistence path from `Docs/Specs/02FrontendQt.md`.

The implementation must use the OS-appropriate application data directory and
the versioned `.bteresult` contract in Spec 07. It must not invent a second
session/result format in an issue or PR.

## Phase 3 - Strategy Comparison

Goal: load saved Backtest Results and compare strategies without requiring every run to be re-executed immediately.

### Phase 3 Required Behavior

- List saved Backtest Results from the application data directory.
- Validate result schema, canonical hash, and referenced data/runtime identities
  before showing them.
- Let the user select at least two saved runs.
- Display key metrics side by side.
- Optionally overlay equity curves when curve data is present.

Minimum comparison fields:

- total return
- CAGR when available
- max drawdown
- win rate
- trade count
- Sharpe or Sortino when available
- final equity

| Issue | Type | Task | Outcome |
| --- | --- | --- | --- |
| [#20](https://github.com/sek788432/Stock-Back-Test-System/issues/20) | FE + BE | Strategy comparison view | Users can compare validated completed Backtest Results by return, drawdown, win rate, trade count, and final equity. |

## Dependency Order

Recommended order for implementation:

1. `#7` and `#8` can start with fake bars because the ownership note says K-line replay is front-end first.
2. `#4`, `#5`, `#6`, and `#18` create the backend replay contract needed by `#9`.
3. `#9` connects the frontend controls and chart to backend replay snapshots.
4. `#10` locks in deterministic behavior before production data is introduced.
5. Replace the historical `#11` scope with immutable Release Snapshot build and
   reader work from Specs 04 and 07.
6. Re-scope `#19` to transactional `.bteresult` persistence and validation.
7. `#12` and `#13` add portfolio, markers, and trade log.
8. `#20` builds comparison UI on top of validated Backtest Results.

## Out Of Scope For Zoe Unless Reassigned

The current organizational guidance is in `Docs/TeamOwnershipAndProductPillars.md`. The following are related product surfaces but should remain separate unless ownership changes:

- full Strategy editor implementation
- full Backtest tab implementation
- full Stock Screener tab implementation
- AI chat to strategy/screener action router
- desktop installer / launcher
