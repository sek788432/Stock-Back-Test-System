# 06 — Indicators

Indicators are project-owned C++ streaming calculations shared by Selectable
Conditions, Python strategies, screening, and Backtest execution. K-line Replay
displays persisted indicator snapshots from a Backtest Result; it does not
recompute executable strategy decisions.

## 1. Delivery status

| Status | Scope |
| --- | --- |
| **Implemented** | `StreamingIndicator` provides validated construction, chronological update, `latest`, consumed-bar count, and reset. It implements SMA, EMA, WMA, RSI, MACD, Bollinger bands, ATR, ADX, stochastic, Donchian, rolling VWAP, OBV, ROC, momentum, true range, and bar-field outputs for the Selectable Conditions path. Its per-bar update uses preconfigured state only. |
| **Planned** | Registry-by-name construction, bounded display history, checkpointing, session-reset VWAP, crossover composition, chart snapshots, and Python exposure. |
| **Blocked** | No indicator may be labelled available until its implementation and required tests land. |

## 2. Contract

- One chronological actual bar updates one symbol's indicator state once.
- Missing bars produce no update and are never forward-filled.
- Warm-up is explicit: executable C++ and Python strategy values are absent (`std::nullopt` / `None`), never a fabricated number or `NaN`.
- Indicators perform no I/O, read no future bars, and do not mutate portfolio or orders.
- Construction validates all parameters and returns `Result`; exceptions do not cross the module seam.
- The engine updates indicators after fills and `on_fill`, but before portfolio marks and `on_market`; see `07` §2.
- Indicator state is owned by one engine run. Multi-run parallelism shares no mutable indicator state.

The planned narrow interface provides update, latest typed value, bounded history, consumed-bar count, identity, and reset/checkpoint support. Implementation classes remain private; a registry constructs built-ins by typed name and arguments.

## 3. V1 catalog

| Kind | Required behavior |
| --- | --- |
| SMA, EMA, WMA | Price moving averages. |
| RSI | Wilder smoothing; default period 14. |
| MACD | Line, signal, and histogram. |
| Bollinger bands | Upper, middle, lower, width, and percent-B. |
| ATR, ADX | Wilder true-range and directional calculations. |
| Stochastic | Percent-K and percent-D. |
| Donchian | Upper, lower, and middle channel. |
| VWAP | Explicit rolling-window or calendar-session reset mode. |
| OBV, ROC, momentum, true range | Volume, rate, difference, and range primitives. |
| Crossover | Above, below, or no-cross event using prior and current values. |
| Bar field | Open, high, low, close, or volume passthrough. |

Crossover composition, Keltner, CCI, MFI, Parabolic SAR, Ichimoku, and
user/plugin indicators are future scope and must remain labelled **Planned**.

## 4. Values, history, and precision

- Internal analytics may use `double`; authoritative accounting types remain the fixed-point types in `07` §7.
- `IndicatorValue` carries both a finite `double` and its price, volume,
  percentage, or scalar domain at the C++ seam.
- Python receives finite `float` values for warmed indicators and `None` before warm-up.
- Selectable Conditions compare typed values and reject incompatible operands at compile time.
- History defaults to a bounded 4,096-value ring for display. An indicator retains any larger internal window needed for correctness.
- Strategy history follows the declared limits in [`05StrategyAuthoring.md`](05StrategyAuthoring.md) §3; exceeding a display ring never changes indicator correctness.
- Rolling algorithms periodically rebase when needed to bound floating-point drift. The chosen formula and rebase interval are versioned because they affect canonical strategy decisions.
- Chart snapshots may use `NaN` only as a rendering gap outside executable strategy interfaces.

Crossing has one definition: `a` crosses above `b` only when the previous comparable values satisfy `a <= b` and the current values satisfy `a > b`; below is the exact mirror. No event occurs without two comparable warmed samples.

## 5. Performance

- Update is O(1) amortized for the V1 catalog.
- No heap allocation is permitted in the per-bar update path after configured buffers are established.
- History requests are read-only; identical Python history requests within one callback are cached.
- Replay seeking restores a compatible checkpoint or deterministically re-warms from actual prior bars.

## 6. Required verification

- Every public indicator behavior requires positive, negative, and boundary unit tests, including parameter validation, exact warm-up boundary, reset/checkpoint, missing bars, and history capacity.
- Numeric fixtures use independently verified expected values and tolerances appropriate to the formula; tests must not reproduce the implementation as their oracle.
- Cross tests must cover equality boundaries and both directions.
- Composition tests prove the same implementation is used by conditions,
  Python, screener, and Backtest execution. Result-replay tests prove persisted
  indicator snapshots are displayed without invoking the indicator scheduler.
- Performance tests cover O(1) update and no post-initialization hot-path allocation.
- Every bug fix or formula/rounding change requires a regression test and, when functional results change, an intentional canonical fixture update.
- A check is merge-blocking only when [`10CiDevFlow.md`](10CiDevFlow.md) marks its implemented gate as required.
