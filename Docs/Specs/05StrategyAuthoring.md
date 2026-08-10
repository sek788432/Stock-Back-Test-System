# 05 — Strategy Authoring

This spec defines how a user strategy becomes commands for the project-owned C++ backtest engine. It is the authority for strategy authoring; execution semantics live in [`07EngineReplayPnL.md`](07EngineReplayPnL.md), and product presentation lives in [`11StockScreenerKLineProduct.md`](11StockScreenerKLineProduct.md).

## 1. Delivery status

| Status | Scope |
| --- | --- |
| **Implemented** | No strategy-authoring mode is implemented. The current application replays OHLCV bars only. |
| **Planned** | Selectable Conditions, Python Script Mode, natural-language proposal flow, validation, Debug Run, versioned templates, and immutable Python runtimes. |
| **Blocked** | No strategy feature may be presented as available until its C++ engine path and required tests exist. Public release also depends on the data-release blockers in `07` §3. |

Status words are factual, not roadmap promises. Only **Implemented** behavior may appear as shipped in user documentation.

## 2. Authoring modes

### 2.1 Selectable Conditions

- The saved source of truth is a versioned, typed condition plan, not Python text.
- Conditions support explicit `all` (AND) or `any` (OR) composition. Nested Boolean groups are planned after V1.
- The compiler validates fields, indicator parameters, symbols, sizing, and actions before a run.
- The plan executes directly in C++ through the same strategy seam as Python commands; it does not invoke an external backtest engine.
- Generated explanatory Python is read-only and non-authoritative. **Edit as Python** creates an independent Python strategy; edits never mutate or reverse-compile into the original condition plan.
- Results retain the canonical plan, plan hash, compiler version, and any generated-source hash.

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

### 2.3 Natural-language assistance

- An assistant may propose a typed condition plan or Python source.
- The UI shows the complete candidate and requires explicit user acceptance before validation or execution.
- Candidate and accepted-artifact identifiers, model identifier when supplied,
  validation outcome, and hashes are retained for traceability. Prompts and
  transcripts are retained only with explicit user consent.
- AI output receives no execution privilege beyond its accepted target mode.

## 3. Strategy context and chronology

`ctx` is a read-only view of the current engine state plus a transactional command buffer:

- `ctx.bars`: actual bars present in the current `MarketSlice`; absent symbols are absent, never forward-filled.
- `ctx.history(...)`: bars no later than the current timestamp.
- `ctx.indicators`: current post-update indicator values; insufficient warm-up is `None`.
- `ctx.portfolio`: read-only positions, cash, margin, and marks.
- `ctx.orders`: order submission, cancellation, replacement, close-position, OCO, and bracket commands.
- `ctx.log`: structured messages retained in results and replay.

Each strategy declares its maximum history requirement. Warm-up reads earlier bars, updates history and indicators, invokes no strategy callback, and permits no orders. Limits are 10,000 bars per symbol and 5,000,000 retained bars across the run. Excess is rejected as `HistoryBudgetExceeded`; the engine never silently truncates a requested window.

`stockbt.strategy` exposes chronology-safe current/past data and never exposes the physical snapshot path. A separate `stockbt.research` interface may inspect the complete immutable snapshot for pandas/NumPy research, but research output is not a verified or replayable backtest result.

## 4. C++ ↔ Python seam

- C++ owns market data, indicator scheduling, validation, orders, fills, accounting, metrics, and persistence.
- Anonymous OS pipes carry lifecycle messages, committed commands, logs, and structured errors.
- Application-owned read-only shared memory carries bulk slices and history arrays.
- TCP, HTTP, localhost ports, and Unix-domain sockets are not used.
- Messages carry protocol version, run ID, callback sequence, and payload hash. Duplicate, mismatched, or out-of-order messages fail with `StrategyProtocolViolation`.
- Commands are buffered for one callback and commit only after a successful return.
- An uncaught callback exception discards that callback's commands and ends the run as `Failed: StrategyException`, retaining hook, timestamp, exception, traceback, and previously committed diagnostic state.
- Cancel first requests cooperative exit. After two seconds, the application terminates the worker process tree, discards the interrupted callback, and saves `Canceled` diagnostic state without final metrics.
- The worker inherits no provider credentials and receives no physical data path.

## 5. Runtime and versioning

- The release contains an immutable, application-managed Python runtime; system Python and arbitrary `pip install` are unsupported.
- V1 packages are `numpy`, `pandas`, `scipy`, `statsmodels`, `scikit-learn`, `matplotlib`, `seaborn`, `pyarrow`, and project-owned `stockbt`.
- Jupyter, TA-Lib, external backtest engines, and duplicate dataframe frameworks are excluded.
- Versions and hashes are locked. Dependency changes create a new `runtimeProfileId`; review occurs quarterly, with tested security fixes expedited through an application release.
- Older profiles remain available while saved strategies reference them. Missing profiles fail as `RuntimeProfileUnavailable`; the application never substitutes a newer profile silently.
- Script API versions use semantic versioning. Migration creates a user-approved copy and diff; it never rewrites the original.
- Default seed `0` initializes Python and NumPy randomness. Arbitrary scripts remain reproducibility-unverified until repeated runs produce the same canonical result hash.

## 6. Validation and Debug Run

**Validate Strategy** checks syntax, allowlisted imports, versions, required hooks, parameters, numeric inputs, and history budgets without creating a verified result.

**Debug Run** executes a small user-selected symbol/date range and stops on the first exception or rejected command. It displays the exact source line and traceback, timestamp, `MarketSlice`, portfolio, indicators, pending orders, buffered commands, and `ctx.log` records. The recorded callback sequence is step-replayable. A fixture may be exported to VS Code or PyCharm; V1 does not embed a line-by-line IDE debugger.

## 7. Required verification

- Every public condition, compiler operation, context member, worker message, hook, and command requires positive, negative, and boundary unit tests.
- Generated conditions require schema tests and golden tests against the C++ execution plan.
- Python requires lifecycle, transactional rollback, protocol violation, exception, timeout, cancellation, history/look-ahead, numeric-normalization, and runtime-version tests.
- Equivalent condition and Python strategies must produce identical canonical engine events when they express the same behavior.
- Every bug fix or intentional public-behavior change requires a regression test that fails against the old behavior.
- A check is merge-blocking only when [`10CiDevFlow.md`](10CiDevFlow.md) marks its implemented gate as required.

Lua and Zipline are not supported strategy runtimes. Native C++ remains an internal/plugin integration surface, not a user strategy language.
