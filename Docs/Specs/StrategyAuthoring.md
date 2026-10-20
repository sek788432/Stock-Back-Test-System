# Strategy Authoring

This spec defines how a user Strategy becomes commands for the project-owned
C++ backtest engine. Execution semantics live in
[`EngineReplayPnL.md`](EngineReplayPnL.md), and presentation lives in
[`BacktestReplayProduct.md`](BacktestReplayProduct.md).

## 1. Delivery status

| Status | Scope |
| --- | --- |
| **Implemented** | An in-memory typed Selectable Conditions plan validates finite thresholds, fields, and indicator definitions; evaluates flat `all` / `any` buy and sell groups over chronological single-symbol bars; and feeds the limited C++ Backtest path. The current Qt page exposes up to two rows per group, including the Task 17 previous UTC data-day close percentage condition. |
| **Accepted design / not implemented** | Task 18 historical gain/extrema/breakout conditions below; saved/versioned plans; up to five buy and five sell conditions; Python Script Mode; Debug Run; immutable managed runtimes; and the worker protocol below. |
| **Blocked** | No strategy feature may be presented as available until its engine path and required tests exist. Public release also depends on the data-release blockers in [`DataLayer.md`](DataLayer.md). |

Status words are factual, not roadmap promises. Only **Implemented** behavior may appear as shipped in user documentation.

## 2. Authoring modes

### 2.1 Selectable Conditions

**Current limited implementation.** A condition compares a bar field, the
percentage close change from the prior actual bar, or one warmed indicator
output to a finite threshold in the same numeric domain (price, volume,
percentage, or scalar). Each buy and sell group has one or two rows and an
explicit `all` / `any` selector. A missing prior close or insufficient
indicator warm-up does not match. The current engine uses the configured whole
share quantity, opens long only, and allows a sell group to close that position
at the next actual open. The plan is not saved yet.

The Task 17 **Previous data-day close change %** metric is implemented in this
limited path. It compares the current actual bar close with the last source
close from the previous available UTC data day. The UI accepts a positive
percentage magnitude and a separate inclusive **Down at least** or **Up at
least** direction. The source close includes extended-hours rows; it is not an
official exchange-session close. The current UTC date is excluded from its own
reference, and absent or zero history does not match. The reference comes from
the same immutable snapshot as the selected execution bars and does not add a
bar to execution or Replay. This source has a distinct Strategy identity:
legacy plans retain their frozen v1 hash framing, while plans using this source
use v2 framing. It remains separate from the signed prior-actual-bar close
change metric.

The Backtest editor shows a unit and an example for every offered threshold.
Price and price-difference values use USD for the current run; close change and
ROC use signed percent points (5 means 5%, not 0.05%); RSI, ADX, and
Stochastic %K show 0–100 oscillator levels rather than a price-change percent;
volume and signed OBV use shares. Display units do not rescale the entered
threshold or alter its typed numeric domain; selecting another metric still
changes the condition's metric normally. A period is an actual-bar
lookback (one hour per bar for hourly runs). Metrics whose formula does not
use the period explain that distinction; the control remains editable and
its typed value continues to participate in the existing strategy identity.
Unit labels do not normalize that metadata.

- The saved source of truth is a versioned, typed condition plan, not Python text.
- The accepted editor supports one to five conditions in each buy and sell
  group, with explicit `all` (AND) or `any` (OR) composition.
- Nested groups, portfolio gates, generated Python, and **Edit as Python** are
  outside scope.
- The compiler validates fields, numeric domains, indicator parameters, symbols, sizing, and actions before a run.
- The plan executes directly in C++ through the same strategy seam as Python commands; it does not invoke an external backtest engine.
- Results retain the canonical plan, plan hash, and compiler version.

#### CSV/UTC-day historical conditions

The limited local workflow will use the existing CSV-derived immutable snapshot
as its historical price standard. A **UTC data day** is a UTC date with actual
source rows. Its close is the last chronological source close, including
extended hours; its high/low are the maximum high/minimum low of those rows.
This is not an official regular-session close or an exchange trading calendar.
Dates without rows do not consume a lookback and no missing row is synthesized.

Task 17's previous-data-day close change predicate is implemented as described
above. The remaining approved Task 18 predicates compare the current K bar's
close with completed prior UTC data days, excluding the entire current UTC date
from historical references:

- An N-day gain uses the last close exactly N available data dates before the
  current date; the approved example is 30 data days.
- Drawdown from a period high and rebound from a period low use prior-day OHLC
  extrema; the one-month preset is the previous 21 completed data days.
- A period-high/low breakout is a strict current-close comparison. Equality
  does not break out; every qualifying K bar matches, not only the first crossing.
- Percentage choices use positive magnitude X and inclusive at-least thresholds.
  Missing, zero or insufficient references are absent/nonmatching, never a
  fabricated value or a future lookup.

Task 18 remains accepted and not implemented. Its pre-range history must warm
only bounded Strategy state and must not become extra executed bars, orders,
fills or Replay candles. New functional fields require versioned Strategy
framing and compatibility review; the existing prior-actual-bar close-change
option and its saved identity remain distinct and must not be reinterpreted.

### 2.2 Python Script Mode

Python is trusted local strategy code executed in a fresh application-managed worker process. It is isolated for application stability, but it is **not a security sandbox**. The application asks for explicit consent on first execution and whenever the script hash changes.

The fixed template declares:

```python
STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"

class Strategy:
    max_history_bars = 200

    def on_start(self, ctx):
        pass

    def on_market(self, ctx):
        bar = ctx.bars.get("AAPL")
        if bar is not None and bar.close > 100.0:
            ctx.orders.limit_buy("AAPL", quantity=10.5, limit_price=99.25)

    def on_fill(self, fill, ctx):
        ctx.log.info("order filled", order_id=fill.order_id)

    def on_corporate_action(self, action, ctx):
        pass

    def on_finish(self, ctx):
        pass


def create_strategy(params):
    return Strategy()
```

Required contract:

- Module function `create_strategy(params)`.
- Returned object method `on_market(ctx)`.
- Declared `STOCKBT_API_VERSION`, template version, and `max_history_bars`.

Optional hooks are `on_start`, `on_fill`, `on_corporate_action`, and `on_finish`. The engine invokes one `on_market` per synchronized `MarketSlice`, not once per symbol.

Python receives ergonomic finite `float` values. Order prices are normalized at the C++ seam to 9 decimal places and quantities to 6; invalid sign, `NaN`, infinity, excess range, or unsupported precision is rejected deterministically. C++ fixed-point values remain authoritative.

## 3. Strategy context and chronology

`ctx` is a read-only view of the current engine state plus a transactional command buffer:

- `ctx.bars`: actual bars present in the current `MarketSlice`; absent symbols are absent, never forward-filled.
- `ctx.history(...)`: bars no later than the current timestamp.
- `ctx.indicators`: current post-update indicator values; insufficient warm-up is `None`.
- `ctx.portfolio`: read-only positions, cash, margin, and marks.
- `ctx.orders`: order submission, cancellation, replacement, close-position, OCO, and bracket commands.
- `ctx.log`: structured messages retained in results and replay.

Each strategy declares its maximum history requirement. Warm-up reads earlier bars, updates history and indicators, invokes no strategy callback, and permits no orders. Limits are 10,000 bars per symbol and 5,000,000 retained bars across the run. Excess is rejected as `HistoryBudgetExceeded`; the engine never silently truncates a requested window.

Project-owned `stockbt` exposes chronology-safe current/past data and never
exposes the physical snapshot path. There is no separate research API.

## 4. C++ ↔ Python seam

- C++ owns market data, indicator scheduling, validation, orders, fills, accounting, metrics, and persistence.
- Anonymous OS pipes carry framed lifecycle messages, committed commands, logs,
  and structured errors. Each frame is a 4-byte unsigned big-endian payload
  length, a 32-byte SHA-256 digest of the payload, then UTF-8 JSON Canonicalization
  Scheme (RFC 8785) bytes.
  The payload maximum is exactly 1 MiB; invalid UTF-8, length, digest, JSON,
  schema, unknown required field, or trailing bytes fail closed as
  `StrategyProtocolViolation`.
- Application-owned read-only shared memory carries bulk slices and history arrays.
- TCP, HTTP, localhost ports, and Unix-domain sockets are not used.
- Messages carry protocol version, run ID, callback sequence, and payload hash. Duplicate, mismatched, or out-of-order messages fail with `StrategyProtocolViolation`.
- Commands are buffered for one callback and commit only after a successful return.
- Every hook has a 30-second deadline. Timeout requests cooperative cancellation
  for two seconds, then terminates the worker process tree and discards the
  interrupted callback.
- An uncaught callback exception discards that callback's commands and ends the run as `Failed: StrategyException`, retaining hook, timestamp, exception, traceback, and previously committed diagnostic state.
- Cancel first requests cooperative exit. After two seconds, the application terminates the worker process tree, discards the interrupted callback, and saves `Canceled` diagnostic state without final metrics.
- The worker inherits no provider credentials and receives no physical data path.

## 5. Runtime and versioning

- The release contains an immutable, application-managed Python runtime; system Python and arbitrary `pip install` are unsupported.
- V1 packages are CPython, project-owned `stockbt`, NumPy, and pandas only.
- User source may import only `stockbt`, `numpy`, `pandas`, `math`, `statistics`,
  `decimal`, `fractions`, `datetime`, `collections`, `itertools`, `functools`,
  `operator`, `bisect`, `heapq`, `dataclasses`, `enum`, `typing`, `random`, and
  `json`. Dynamic imports and non-allowlisted system, process, filesystem,
  native-extension, introspection, and network modules are rejected at
  validation. Dependencies
  internal to the immutable runtime are not user-import permissions.
- The allowlist is a reproducibility boundary, not a security sandbox.
- Versions and hashes are locked. Dependency changes create a new `runtimeProfileId`; review occurs quarterly, with tested security fixes expedited through an application release.
- Strategies in both Active and Trash pin their Runtime Profile until the
  Strategy is permanently purged. Unreferenced profiles move to recoverable
  Runtime Trash and may be permanently removed after 30 days. Results do not
  pin profiles because replay never executes source.
- Script API versions use semantic versioning. Migration creates a user-approved copy and diff; it never rewrites the original.
- Default seed `0` initializes Python and NumPy randomness. A normal run executes
  once and is labeled reproducibility-unverified. An explicit reproducibility
  check runs the same immutable inputs three times in fresh workers and is
  verified only when all canonical result hashes match.

## 6. Validation and Debug Run

**Validate Strategy** checks syntax, allowlisted imports, versions, required hooks, parameters, numeric inputs, and history budgets without creating a verified result.

**Debug Run** accepts at most five symbols, at most 500 post-warm-up callbacks,
and at most 30 seconds wall time. It stops on the first exception, timeout, or
rejected command and produces diagnostics only—never a verified `.bteresult`.
It displays the source line and traceback, timestamp, `MarketSlice`, portfolio,
indicators, pending orders, buffered commands, and structured logs.

## 7. Required verification

- Every public condition, compiler operation, context member, worker message, hook, and command requires positive, negative, and boundary unit tests.
- Selectable Conditions require schema tests and golden tests against the C++ execution plan.
- Python requires lifecycle, transactional rollback, protocol violation, exception, timeout, cancellation, history/look-ahead, numeric-normalization, and runtime-version tests.
- Equivalent condition and Python strategies must produce identical canonical engine events when they express the same behavior.
- Every bug fix or intentional public-behavior change requires a regression test that fails against the old behavior.
- A check is merge-blocking only when [`CiDevFlow.md`](CiDevFlow.md) marks its implemented gate as required.

Lua, Zipline, native plugins, and AI authoring are unsupported.
