# K-Line Replay Roadmap

This document tracks the implementation plan for Zoe's K-line replay ownership area.
It is a planning map for GitHub issues, not a replacement for the product specs.

Authoritative specs:

- `Docs/Specs/02_Frontend_Qt.md`
- `Docs/Specs/04_Data_Layer.md`
- `Docs/Specs/07_Engine_Replay_PnL.md`
- `Docs/Specs/11_Stock_Screener_KLine_Product.md`

Current repository state: `main` has backend core and data-fetcher code, but no Qt frontend, data read layer, or replay engine implementation yet.

## Phase 1 - K-Line Replay MVP

Goal: deliver a visible, testable replay loop that can read fixture/CSV bars, step or play through them, and render K-line candles in the Qt UI. Backend integration may use fake or fixture-backed components until production data and strategy execution are ready.

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

Goal: connect replay to production-style data and persist completed run results for later comparison. File-system result snapshots are separate from in-memory progress snapshots used by the UI.

| Issue | Type | Task | Outcome |
| --- | --- | --- | --- |
| [#11](https://github.com/sek788432/Stock-Back-Test-System/issues/11) | BE | Read-only DuckDB `DataSource` | Replay can read real OHLCV rows from `StockData/MarketData.duckdb` without writing to it. |
| [#19](https://github.com/sek788432/Stock-Back-Test-System/issues/19) | BE | Persist replay result snapshots | Completed runs are saved to the file system for future strategy comparison. |
| [#12](https://github.com/sek788432/Stock-Back-Test-System/issues/12) | BE | Portfolio snapshots and trade markers | Replay can emit cash, position, equity, PnL, and buy/sell marker data. |
| [#13](https://github.com/sek788432/Stock-Back-Test-System/issues/13) | FE | Portfolio strip and trade log | Replay UI displays portfolio state, trade rows, and chart markers. |

Persisted result snapshots should include enough metadata to compare runs later:

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
- generated timestamp
- data snapshot or data version metadata when available

## Phase 3 - Strategy Comparison

Goal: load saved result snapshots and compare strategies without requiring every run to be re-executed immediately.

| Issue | Type | Task | Outcome |
| --- | --- | --- | --- |
| [#20](https://github.com/sek788432/Stock-Back-Test-System/issues/20) | FE + BE | Strategy comparison view | Users can compare saved snapshots by return, drawdown, win rate, trade count, and final equity. |

## Dependency Order

Recommended order for implementation:

1. `#7` and `#8` can start with fake bars because the ownership note says K-line replay is front-end first.
2. `#4`, `#5`, `#6`, and `#18` create the backend replay contract needed by `#9`.
3. `#9` connects the frontend controls and chart to backend replay snapshots.
4. `#10` locks in deterministic behavior before production data is introduced.
5. `#11` replaces fixture/CSV data with read-only DuckDB data.
6. `#19` persists completed results for comparison.
7. `#12` and `#13` add portfolio, markers, and trade log.
8. `#20` builds comparison UI on top of saved result snapshots.

## Out Of Scope For Zoe Unless Reassigned

Based on `Docs/owner.md`, Zoe owns K-line replay front end first, then back end. The following are related product surfaces but should remain separate unless ownership changes:

- full Strategy editor implementation
- full Backtest tab implementation
- full Stock Screener tab implementation
- AI chat to strategy/screener action router
- desktop installer / launcher
