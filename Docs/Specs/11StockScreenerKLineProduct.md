# 11 — K-Line Replay, Strategy Modes, and Stock Screener

This spec is the user-visible product contract. Strategy details live in [`05StrategyAuthoring.md`](05StrategyAuthoring.md), indicators in [`06Indicators.md`](06Indicators.md), and execution/results in [`07EngineReplayPnL.md`](07EngineReplayPnL.md).

## 1. Honest delivery status

| Capability | Status | User-visible truth |
| --- | --- | --- |
| Bar loading and K-line playback | **Implemented with known gap** | Load tracked hourly CSV bars, aggregate them to daily bars, show candlesticks/volume, and play/pause/step/change speed. Other displayed timeframe choices are not supported and may currently receive hourly data; this must be fixed before claiming general timeframe selection. |
| Portfolio strip in current replay | **Implemented placeholder** | Displays initial cash/equity only; it is not driven by trades. |
| Legacy replay summaries | **Implemented legacy** | JSON summary save/list/compare exists; it is not the target `.bteresult` backtest artifact. |
| Starter Backtest page | **Implemented limited slice** | Run the compatibility fixed market buy or Selectable Conditions over one symbol/range. All orders are whole-share, long-only market orders eligible only at the next actual bar open with 1 bp adverse slippage. The page shows recorded buy/sell fills and the final mark. This is not the complete strategy, order, metric, or result workflow. |
| Selectable Conditions | **Implemented limited slice** | The Backtest page offers up to two typed buy and two typed sell predicates with flat ALL/ANY composition. Predicates cover bar fields, close-change percentage, and the currently implemented technical indicators. Saved artifacts, nested groups, portfolio gates, sizing, richer actions, and Screener reuse remain planned. |
| Python Script Mode and Debug Run | **Planned** | Trusted worker design is specified; no shipped worker/runtime yet. |
| Natural-language assistance | **Planned** | Candidate generation with explicit user acceptance; no shipped integration yet. |
| Complete orders, fills, P&L, metrics, result replay | **Planned** | Only the explicitly limited starter Backtest slice exists; Replay remains bar-only and does not execute the engine. |
| Stock screener | **Planned** | No shipped universe filtering/ranking workflow. |
| Public data-bearing release | **Blocked** | Requires documented redistribution rights and verified redistribution-cleared split metadata. |

The UI, README, release notes, and screenshots must use these status meanings and must not call the current bar player a complete backtest.

## 2. Backtest execution and K-line Replay

The completed **Backtest setup** accepts an ordered universe from the immutable
Release Snapshot, supported timeframe/range, positive initial capital, Strategy,
costs, risk-free rate, and runtime/numeric profiles. Batch and Paced Backtest
execution submit that same Run Configuration to the project-owned C++ engine.
For identical inputs, they produce the same canonical events and result hash;
pause, step, and speed controls affect wall-clock pacing only.

**K-line Replay** opens an existing `.bteresult` and its exact retained Data
Segments. It does not accept or execute a new Strategy, call Python hooks,
reevaluate orders, or create fills. Playback shows persisted candlesticks,
volume, fills, corporate actions, indicator snapshots, ambiguity/warning markers,
cash, restricted cash, positions, margin, realized/unrealized P&L, equity, trade
log, and structured strategy logs. Controls include play, pause, single-record
step, supported seek, speed presets, and maximum speed; these controls only
change presentation.

If required data, runtime, or a valid final mark is unavailable, the UI displays the exact structured status from `07`; it never fabricates bars, fills, or completed metrics.

## 3. Strategy experience

### 3.1 Selectable Conditions

- Form rows use typed fields, indicators, comparisons, portfolio gates, sizing, and actions.
- V1 offers flat **ALL (AND)** or **ANY (OR)** composition. Nested groups are **Planned**.
- The canonical condition model is saved and executed in C++.
- Generated explanatory Python is read-only. **Edit as Python** creates a separate strategy.

### 3.2 Python Script Mode

- The editor starts from the fixed versioned template in `05` §2.2.
- **Validate Strategy** reports contract and input problems before execution.
- **Debug Run** uses a small chosen range and shows source traceback, slice, portfolio, indicators, orders, commands, and structured logs.
- The UI clearly states that Python is trusted local code isolated for stability, not a secure sandbox.
- Users select only project-managed data for verified runs; V1 does not import user pricing data.

### 3.3 Natural-language assistance

- The assistant produces a complete candidate condition plan or Python script.
- Nothing runs until the user reviews and explicitly accepts the candidate.
- Candidate/model identity when supplied, accepted artifact, validation outcome,
  and hashes remain attached for support and reproducibility. Prompts and
  transcripts are retained only with explicit user consent.

## 4. Stock screener contract

The screener evaluates only bars at or before an explicit as-of timestamp and outputs a filtered/ranked symbol list, not orders.

- It reuses the same typed indicators and flat ALL/ANY condition model as strategy authoring.
- An accepted Python screener uses the same runtime, trust notice, validation, history limits, and chronology-safe data interface as Python strategies.
- Natural-language input remains a proposal path, never a separate execution engine.
- V1 universe sources are only those enumerated in the immutable project snapshot; watchlists or index membership require snapshot metadata.
- Minimum result fields are rank, symbol, as-of timestamp, last actual price, and each condition/rank value. Company name, sector, market cap, and benchmark attribution appear only when verified snapshot fields exist.
- CSV/clipboard export preserves the as-of timestamp, universe/snapshot ID, condition artifact hash, and generator/runtime version.

## 5. Local artifacts and lifecycle

- Strategies, condition plans, metadata, runtime references, and `.bteresult` files live in the OS-appropriate application data directory, never inside the installation directory.
- Results reference immutable project data segments; users cannot replace their bars.
- Deletion uses the 30-day Trash and reference rules in `07` §8.
- Runtime upgrades and strategy-template migrations never silently rewrite an existing strategy or result.
- User documentation includes one complete runnable Python template and plain explanations of next-bar activation, full-fill-on-touch, OCO adverse-first ambiguity, extended-hours execution, missing bars, splits, excluded dividends, short-margin assumptions, transaction costs, and incomplete results.

## 6. Required verification

- Every public UI/view-model behavior requires positive, negative, and boundary unit tests, including accessibility and queued worker-to-UI delivery.
- End-to-end fixtures cover condition and Python authoring, validation/Debug
  Run, Batch/Paced Backtest parity, result reopening without engine execution,
  screener no-look-ahead, and all incomplete/blocked states.
- Current bar-only replay tests remain truthful and must not use placeholder portfolio values as evidence of engine accounting.
- Every bug fix or intentional user-visible behavior change requires a regression test.
- A check is merge-blocking only when [`10CiDevFlow.md`](10CiDevFlow.md) marks its implemented gate as required.

## 7. Explicitly out of scope for V1

- Lua, Zipline, or another third-party backtest engine.
- User pricing-data import, runtime provider downloads, or database integration.
- Live brokerage execution, partial fills, order-book liquidity, stop-limit, IOC, FOK, and dividend accounting.
- Arbitrary Python package installation, system Python, or cloud execution of strategies.
- Benchmark alpha/beta and nested condition groups.
