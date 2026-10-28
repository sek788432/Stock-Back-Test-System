# Strategy Authoring

This spec defines how a user Strategy becomes commands for the project-owned
C++ backtest engine. Execution semantics live in
[`EngineReplayPnL.md`](EngineReplayPnL.md), and presentation lives in
[`BacktestReplayProduct.md`](BacktestReplayProduct.md).

## 1. Delivery status

| Status | Scope |
| --- | --- |
| **Implemented** | An in-memory typed Selectable Conditions plan validates finite thresholds, fields, and indicator definitions; evaluates flat `all` / `any` buy and sell groups over chronological single-symbol bars; and feeds the limited C++ Backtest path. The current Qt page exposes up to two rows per group, including Task 17 and Task 18 UTC data-day conditions. |
| **Implemented — Python milestone code** | The Strategies editor, sample, bounded `.py` import/export, atomic recoverable draft, managed-worker Validate action, frozen Backtest handoff, source-hash consent, one-run execution path, durable three-run verification evidence/coordinator, schema-3 provenance, and schema-2 copy-migration backend are implemented and registered tests pass. |
| **Implemented — optional macOS runtime bundle** | The exact pinned CPython 3.14.9 profile and package wheels assemble offline, pass package import checks, and can be embedded in a macOS Apple Silicon `.app`. CMake/CTest verify the bundle and execute a Python worker against that profile. The default developer build omits the profile. |
| **Planned — release acceptance** | Signed installer integration, manual edit-to-Replay acceptance, and release-baseline verification remain required before calling this a supported user release. |
| **Accepted design / not implemented** | Saved/versioned plans; up to five buy and five sell conditions; the Results migration UI action; signed full installer; Debug Run; and Windows/Linux runtime acceptance. |
| **Blocked — public release** | Public data-bearing release still depends on the redistribution and split-manifest blockers in [`DataLayer.md`](DataLayer.md). |

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

Task 18 is implemented in the limited local path. **Up X% over selected
period** defaults to 30 available data days and compares the current close with
the close exactly N available data dates earlier. The high drawdown, low
rebound, and strict high/low breakout choices default to 21 completed data
days; they compare the current close with the maximum high or minimum low from
those prior dates. A breakout remains true on every qualifying bar rather than
only the first crossing. The whole current UTC date is excluded from these
references, including rows earlier in the selected run. The editor accepts a
positive percentage magnitude for inclusive drawdown/rebound/gain thresholds;
breakouts use strict comparisons and no threshold. Each period is part of the
typed condition and its canonical identity.

The Backtest binding requests only the bounded history needed from the same
immutable snapshot. It seeds Strategy state without adding bars, indicator
updates, orders, fills, portfolio records, or Replay candles. Legacy plans keep
their frozen v1 hash framing, Task 17 plans use v2, and plans containing Task 18
sources use v3 framing that includes the selected period. The existing
prior-actual-bar close-change option remains distinct and unchanged.

### 2.2 Python Script Mode

The current application exposes the first-milestone editor and Backtest
actions. Configure with `BTE_BUNDLE_MANAGED_PYTHON_RUNTIME=ON` to assemble the
locked profile offline and embed it in the macOS Apple Silicon app; users of
that build do not need to install Python. The default developer build leaves
the option off and reports the managed runtime unavailable. Signed installer
and manual release acceptance are tracked separately.

Python is trusted local strategy code executed in a fresh application-managed
worker process. The separate process protects application stability; it is
**not a security sandbox**. The first Python milestone supports the narrow
single-symbol, hourly, long-only market-order workflow below. Limits in this
subsection are normative for that milestone; unsupported features fail
validation or the entire callback explicitly and are never silently ignored.

The fixed template is:

```python
import stockbt

STOCKBT_API_VERSION = "1"
STOCKBT_TEMPLATE_VERSION = "1.0.0"


class SmaCross:
    def __init__(self, params):
        self.fast_period = params["fast_period"]
        self.slow_period = params["slow_period"]
        self.quantity = params["quantity"]
        self.max_history_bars = self.slow_period
        self.indicators = {
            "fast": stockbt.SMA("fast", period=self.fast_period),
            "slow": stockbt.SMA("slow", period=self.slow_period),
        }
        self.previous_fast = None
        self.previous_slow = None

    def on_start(self, ctx):
        pass

    def on_market(self, ctx):
        fast = ctx.indicators["fast"]
        slow = ctx.indicators["slow"]
        if fast is None or slow is None:
            return

        position = ctx.portfolio.positions.get(ctx.symbol)
        is_long = position is not None and position.quantity_shares > 0
        crossed_up = (
            self.previous_fast is not None
            and self.previous_slow is not None
            and self.previous_fast <= self.previous_slow
            and fast > slow
        )
        crossed_down = (
            self.previous_fast is not None
            and self.previous_slow is not None
            and self.previous_fast >= self.previous_slow
            and fast < slow
        )

        if not is_long and crossed_up:
            ctx.orders.buy_market(ctx.symbol, quantity=self.quantity)
        elif is_long and crossed_down:
            ctx.orders.close_position(ctx.symbol)

        self.previous_fast = fast
        self.previous_slow = slow

    def on_fill(self, fill, ctx):
        ctx.log.info(
            "order filled",
            side=fill["side"],
            quantity_shares=fill["quantity_shares"],
        )

    def on_finish(self, ctx):
        pass


def create_strategy(params):
    return SmaCross(params)
```

The required module declarations are `STOCKBT_API_VERSION == "1"`,
`STOCKBT_TEMPLATE_VERSION == "1.0.0"`, and `create_strategy(params)`. The
factory returns an object with callable `on_market(ctx)`, an integer
`max_history_bars` from 1 through 10,000, and an `indicators` mapping of unique
ASCII names to `stockbt.SMA(name, period)` declarations. Names match
`[A-Za-z][A-Za-z0-9_]{0,63}`. SMA periods are integers from 1 through 10,000;
booleans are not integers for this contract. The declared history budget must
be at least the largest registered period. Indicator registration is read
once from the returned object after factory completion and cannot change in a
callback. The one available indicator is C++ streaming SMA of actual-bar
close. It includes the current bar after the indicator update and returns
`None` until its period is warmed.

Only `on_start(ctx)`, required `on_market(ctx)`, optional `on_fill(fill, ctx)`,
and optional `on_finish(ctx)` are supported. `on_corporate_action` and any
other lifecycle hook are rejected as `StrategyContractViolation`; they are
not silently skipped. `on_start` runs once before warm-up with no current bar.
Warm-up reads at most `max_history_bars` actual pre-range bars from the same
immutable snapshot, updates history and indicators, and invokes no user hook.
For each execution bar the Engine evaluates prior orders and applies fills,
delivers resulting fills to `on_fill`, updates indicators and portfolio marks,
then invokes `on_market` exactly once. `on_finish` runs only after a natural
Engine completion. The fill and finish hooks may log and inspect immutable
context but cannot submit orders. Hook order and the timestamp each hook sees
are part of the Strategy API contract.

`on_market` may submit at most one command: `ctx.orders.buy_market(symbol,
quantity)` or `ctx.orders.close_position(symbol)`. The first opens a long
position only; the second closes the entire current long position. Quantity is
a positive whole-share integer no greater than 1,000,000,000, matching the
existing Engine bound. The symbol must equal `ctx.symbol`, and the run selects
one symbol. Commands become eligible at the next actual bar open and never
fill on the signal bar. Short selling, fractional quantities, limits, stops,
OCO/brackets, pending-order management, symbol-universe strategies, and
user-defined price orders are unsupported. A second command, a close without
a long position, an unsupported method, or an invalid value fails as
`StrategyCommandRejected` and discards every command buffered for that callback.

Run parameters are an immutable JSON object limited to 64 KiB encoded by RFC
8785, at most 64 keys, 256 UTF-8 bytes per key, 4 KiB per UTF-8 string,
nesting depth 8, and finite JSON numbers only. The first UI supplies exactly
`fast_period`, `slow_period`, and
`quantity` as integers; defaults are 5, 20, and 1, periods are 1–10,000 with
`fast_period < slow_period`, and quantity is 1–1,000,000,000. Parameters are
validated before a run and frozen with source before consent or worker launch.
Imported `.py` files are limited to 1 MiB of raw bytes before decoding. After
strict UTF-8 decoding, optional removal of one leading BOM, and LF newline
normalization, the execution source is limited to 1 MiB again. Malformed UTF-8,
NUL bytes, and files exceeding either limit are rejected without replacing the
current editor buffer. The execution source is the normalized UTF-8 text shown
in the editor, and that exact text is embedded in Results.

Source consent is keyed to the SHA-256 of the exact normalized execution
source plus API version. The consent dialog shows that identity and states
that the script is trusted local code, may access the user's normal account,
and is not sandboxed. Consent is required before any user module or factory
code is evaluated, including dynamic validation, the first time and whenever
that identity changes. A Run captures immutable source and parameters before
showing consent; later edits belong to the next run and cannot change the
active execution.

### 2.3 Strategy context and chronology

`ctx` and every value reachable from it are read-only immutable values:

- `ctx.symbol` is the selected symbol. `ctx.timestamp_utc_ms` is the current
  event time in UTC milliseconds, or absent during `on_start`.
- `ctx.bar` is the current actual bar during `on_market`; `ctx.bars` maps only
  symbols with an actual bar in the current slice and is a one-symbol mapping
  in the first milestone. No missing hour is synthesized or forward-filled.
- Bar values are finite numeric values (`open`, `high`, `low`, `close`, and
  volume); cash and prices remain Engine-owned fixed-point values and are
  converted to finite Python floats only for observation.
- `ctx.history(symbol, *, count, end_utc_ms=None)` returns an immutable tuple
  in chronological order of the latest actual bars at or before the inclusive
  `end_utc_ms`. The default end is the current UTC timestamp. A supplied end
  is a signed UTC Unix-millisecond integer no later than the current timestamp;
  booleans and non-integers are rejected, and a future end fails as
  `FutureDataRequest`. Calling history from `on_start`, when there is no current
  timestamp, fails as `HistoryUnavailable`. `count` must be a positive integer
  excluding booleans; missing/invalid symbol, non-integer end, or a nonpositive
  count fails as `InvalidHistoryRequest`.
  A count greater than either `max_history_bars` or 10,000 fails as
  `HistoryBudgetExceeded`. If fewer source bars exist, the tuple contains those
  available bars; no synthetic rows are returned. Pre-range bars are visible
  only through history and indicators, never as execution, Result, or Replay
  bars.
- `ctx.indicators` contains the registered C++ indicator values after the
  current-bar update; an un-warmed value is `None`.
- `ctx.portfolio` contains immutable cash, equity, and per-symbol position
  snapshots, including integer whole-share quantity and optional actual mark
  price. No mutable Engine object, file path, provider credential, or shared
  writable buffer is exposed.
- `ctx.log.info(message, **fields)` and corresponding `warning` and `error`
  methods append bounded structured diagnostics. `message` is valid UTF-8 text
  up to 8 KiB. Field keys are valid UTF-8 text up to 256 bytes; values are
  finite JSON scalars, arrays, or objects to nesting depth 4. Logs never
  become orders or alter a canonical fill.

History is always read from the current immutable snapshot and never later
than the current event. Requested windows over the declared budget, 10,000
bars per symbol, or the run-wide 5,000,000 retained-bar limit fail as
`HistoryBudgetExceeded`; the Engine never silently truncates a requested
window. No DataFrame history accessor is part of API version 1. Users may
construct their own pandas values from the read-only bars they requested.

### 2.4 C++ ↔ Python seam

- C++ owns market data, indicator scheduling, Strategy validation, command
  acceptance, orders, fills, accounting, metrics, and persistence. The Engine
  receives validated typed commands; process management stays in Bindings / a
  dedicated Runner module and Qt stays in Frontend.
- Dedicated anonymous OS pipes carry protocol input and output. Script
  `stdout` and `stderr` use separate bounded diagnostic pipes and cannot
  corrupt protocol frames. Application-owned shared memory carries normalized
  source bytes and bulk bars/history; the worker receives read-only handles,
  and the pipe envelope contains their bounded descriptors, lengths, and
  hashes rather than escaping source text into JSON. TCP, HTTP, localhost
  ports, and Unix-domain sockets are not used.
- Each protocol frame is a 4-byte unsigned big-endian payload length, a
  32-byte SHA-256 digest of the payload, then UTF-8 RFC 8785 JSON bytes. The
  maximum payload is exactly 1 MiB. The closed envelope contains protocol
  version, run ID, callback sequence, message type, and payload; unknown keys,
  duplicate keys, bad UTF-8, non-canonical JSON, length/hash mismatch,
  trailing bytes, wrong identity, and duplicate or out-of-order sequence fail
  closed as `StrategyProtocolViolation`.
- A callback's entire command set is staged in the parent and committed only
  after a complete valid success response. Exception, invalid command,
  protocol failure, cancellation, or timeout discards all commands from that
  callback and retains earlier committed Engine slices.
- Parent-enforced startup deadlines are 10 seconds from process creation to
  bootstrap-ready, 5 seconds for protocol handshake, 10 seconds for module
  evaluation, and 10 seconds for `create_strategy(params)` plus contract
  inspection. These are separate monotonic-clock phases, not covered by the
  30-second deadline for each user hook. Parent-side timers remain authoritative
  even if the child is CPU-bound or cannot process cancellation.
- A startup timeout is respectively `WorkerBootstrapTimeout`,
  `WorkerHandshakeTimeout`, `StrategyLoadTimeout`, or
  `StrategyFactoryTimeout`; non-timeout errors retain the corresponding
  `WorkerStartFailed`, `WorkerHandshakeFailed`, `StrategyLoadFailed`, or
  `StrategyFactoryFailed` classification. Contract errors use
  `StrategyContractViolation`. Each hook has a 30-second deadline; timeout is
  `StrategyHookTimeout`. Startup and factory phases complete before the Result
  writer is created. Their failure/cancellation leaves no staged, draft, or
  published Result. `on_start` and later hooks occur after writer creation;
  their terminal failures retain the Result's trusted committed prefix and
  diagnostics under the existing explicit Save/Discard lifecycle.
- Cancel and callback completion are linearized in the parent. If Cancel is
  accepted before a callback's valid completion is committed, cancellation
  wins and that callback's commands and slice are discarded. If completion is
  committed first, that slice remains durable and cancellation prevents the
  next callback. The same first-event rule applies to timeout and completion.
  Cancellation or timeout requests cooperative shutdown for two seconds,
  then terminates the worker process tree. Cleanup closes every pipe and
  shared-memory handle. A canceled/failed run has no valid final performance
  metrics.
- Diagnostic retention is bounded to 10,000 structured entries and 1 MiB of
  combined canonical log bytes per execution. A single hook response retains
  at most 512 KiB of canonical structured logs before C++ applies the run-wide
  limit. Each captured raw `stdout` and `stderr` stream is capped at 256 KiB.
  On overflow, retain the first and last halves, count omitted bytes/entries,
  append one `DiagnosticsTruncated` notice, and keep draining/discarding later
  output so the worker cannot block on a full pipe. Worker streams are stored
  as base64 byte payloads so arbitrary bytes remain canonical JSON. Logging
  overflow alone does not fail the strategy. A single log message is at most
  8 KiB.
- Schema-3 Results persist bounded logs and typed terminal diagnostics. A
  terminal record carries the stable error code, hook or phase, message,
  optional source line/column, bounded traceback, and omission counts. Python
  exceptions include logs emitted before the exception. Parent-detected
  cancellation, timeout, protocol failure, and callback validation failure
  also receive typed terminal records. If cancellation, a deadline, malformed
  diagnostics, or a retention limit means the complete stream cannot be
  established, `diagnosticComplete` is false; the Engine emits at most one
  `DiagnosticsTruncated` notice before terminal diagnostics.
- The worker inherits no provider credentials and receives no physical data
  path. Process separation and import restrictions protect stability and
  reproducibility; neither is a security sandbox.

At the C++ seam, `Strategy::onMarket` returns one complete command batch to the
Engine. The batch contains zero or one command; the Engine rejects an unknown
kind, mismatched symbol, invalid quantity, second open while already long, or
close while flat as `StrategyCommandRejected` before adding any command or
current-bar records. The immutable C++ context carries the selected symbol,
optional current actual bar, fixed-point cash/equity/position values, and
optional latest mark price. `onStart` receives no bar or mark. `onFill`
receives the applied fill and post-fill cash/position with the latest prior
mark, before the current bar's close mark is updated; `onMarket` receives the
current-bar close mark. `onFinish` runs after the last current-bar mark only
when the Engine reaches a natural completion. The shared value-only interface
does not expose Engine objects or process handles.

### 2.5 Runtime and versioning

- The macOS first-milestone profile is an application-managed immutable
  CPython 3.14.9 build from Astral's `python-build-standalone` release
  `20261021`, target `aarch64-apple-darwin`, standard GIL-enabled
  `install_only` archive. Its SHA-256 is
  `226d696642a8a53d03b4d5b255b1c319b1575a6d44bd723a953c9f507755e36a`.
  The supported release baseline is macOS 14 or later on Apple Silicon. The
  immutable archive is included in the signed application; no runtime network
  fetch or system-Python fallback is permitted.
- The profile pins NumPy 2.5.4
  (`numpy-2.5.4-cp314-cp314-macosx_11_0_arm64.whl`, SHA-256
  `ffa6ce09a1c6a08e9667dd9c97aa0b14184e8d18f2a14b78b2a2328c9147f076`),
  pandas 3.0.6 (`pandas-3.0.6-cp314-cp314-macosx_11_0_arm64.whl`, SHA-256
  `ff51a4459ed036e93d1eb1bb5e6e7b28685d3cb6b7c12b91c05b31024e234729`),
  python-dateutil 2.9.0.post0 (SHA-256
  `a8b2bc7bffae282281c8140a97d3aa9c14da0b136dfe83f850eea9a5f7470427`),
  six 1.17.0 (SHA-256
  `4721f391ed90541fddacab5acf947aa0d3dc7d27b2e1e8eda2be8970586c3274`),
  and IANA `tzdata` 2026.5 (SHA-256
  `b683bd1b6659ddcd810ff02ad09ba821d4bf1065072805063eb35c49617905ac`).
  `tzdata` is pinned even though pandas only declares it for Windows and
  Emscripten: the Worker configures Python's zone database from this profile
  so timezone behavior does not silently depend on the host database. These
  hashes identify the selected upstream files; P2 must download, verify,
  install offline, import-test on the supported Mac, and record the completed
  profile manifest hash before claiming runtime compatibility.
- The package license set is CPython PSF-2.0 and each bundled-library notice
  carried by the exact standalone archive; NumPy BSD-3-Clause with its bundled
  component notices; pandas BSD-3-Clause; python-dateutil Apache-2.0 OR
  BSD-3-Clause; six MIT; tzdata Apache-2.0; and project-owned `stockbt` under
  the repository license. The distribution must retain all upstream notices
  and ship a generated SPDX SBOM. The exact standalone archive's license files
  and all package `METADATA` license fields are checked before release; any
  unknown or disallowed component blocks packaging. The runtime builder uses
  no runtime `pip install`, network resolver, or source-build fallback.
- User import roots are `stockbt`, `numpy`, `pandas`, `math`, `statistics`,
  `decimal`, `fractions`, `datetime`, `zoneinfo`, `collections`, `itertools`,
  `functools`, `operator`, `bisect`, `heapq`, `dataclasses`, `enum`, `typing`,
  `random`, and `json`. Dynamic imports and non-allowlisted system, process,
  filesystem, native-extension, introspection, and network modules are
  rejected by validation. Direct or reflective access to `__builtins__` or
  `__import__` and dynamic code operations such as `eval`, `exec`, and
  `compile` are rejected before module evaluation. Namespace and attribute
  reflection builtins (`globals`, `locals`, `vars`, `getattr`, `hasattr`,
  `setattr`, `delattr`, and `dir`) are also rejected before module evaluation.
  Dunder attribute access and the attribute-reflection helpers
  `operator.attrgetter` and `operator.methodcaller` are not permitted. Dynamic
  annotation evaluation through `typing.ForwardRef`,
  `typing.evaluate_forward_ref`, or `typing.get_type_hints` is also prohibited.
  Strategy `from` imports cannot use star imports or import reflective names.
  Imports use a fixed worker allowlist that checks the imported root again at
  runtime. Dependencies imported internally by the immutable runtime are not
  user-import permissions. This allowlist is a reproducibility boundary, not a
  security sandbox.
- Each immutable runtime manifest records exact package versions, upstream
  URLs, SHA-256 values, license identifiers/notices, platform, architecture,
  Python ABI, stockbt API and source hash, installed-file hashes, and profile
  builder version. `runtimeProfileId` names the manifest and
  `runtimeProfileHash` is SHA-256 over its canonical bytes. Any component,
  build, license, or `stockbt` change creates a new identity. Release CI builds
  from a hash-locked local artifact cache with networking disabled, verifies
  every input hash, rejects source builds, then imports `stockbt`, NumPy,
  pandas, and pinned timezone data under the bundled interpreter. At startup,
  the app verifies the installed profile manifest and file hashes before
  Validate or Run; missing/tampered profiles fail clearly. Replay never loads
  or verifies Python.
- Versioned Python Strategy library/profile trash is outside the first
  milestone. The editor retains one recoverable draft and `.py` files only;
  the current app installer owns its immutable default profile. A future
  saved Strategy library must pin referenced profiles before profile removal
  policy is introduced. Results record profile identity and hash but do not
  pin profiles because replay never executes source.
- API version `1` is fixed for this narrow command/context contract. Template
  version changes whenever the shipped sample contract changes. API or result
  hash framing changes require an explicit version change and a reviewed
  compatibility path; user source is never silently rewritten.
- Python's `random` module and NumPy's legacy random generator are seeded with
  integer 0 before user module evaluation. A normal Run executes once and is
  labeled reproducibility-unverified. Explicit Verify Reproducibility freezes
  all functional inputs and runs three fresh workers sequentially; only three
  completed runs with identical canonical functional hashes are verified.

### 2.6 Validation, drafts, and diagnostics

**Validate Strategy** performs strict UTF-8/size checks, static syntax and
import-policy checks, API/template and required-hook checks, parameter and
history-budget checks, then evaluates module top-level code and the factory in
a fresh managed worker to verify the returned contract. User code is never
executed in the App or Engine process. Dynamic validation requires the same
hash-scoped consent as Run, uses the startup deadlines above, creates no
Result or verification evidence, and cannot submit Engine commands.

The first Strategies page has one recoverable editor draft containing the
normalized source and the three UI parameters; it is not a versioned Strategy
library and is not tied to a data selection. The draft is atomically replaced
after a 500 ms idle debounce, on page transition, and on orderly shutdown. The
draft has one validated previous generation for crash recovery. A corrupt
latest generation is preserved for diagnosis and the previous valid copy is
loaded with a warning; if neither validates, the editor opens empty without
overwriting either file until the user saves or explicitly resets. Load and
dirty-buffer replacement offer **Save Draft**, **Discard**, and **Cancel**.
Load accepts only `.py`, performs strict UTF-8 normalization and the 1 MiB
limit before changing the buffer, and preserves the previous draft generation.
Export writes the exact editor source as UTF-8/LF using an atomic replace.
Oversized or malformed input never partially replaces the draft.

Stable input and runtime error codes are `InvalidStrategyEncoding` for
malformed UTF-8, `StrategySourceTooLarge` for source over 1 MiB,
`StrategySourceContainsNul` for a NUL byte, `StrategySyntaxError` for invalid
Python syntax, `StrategyImportNotAllowed` for a forbidden or dynamic import,
`UnsupportedStrategyApiVersion` for an unsupported API declaration,
`InvalidStrategyParameters` for invalid parameter JSON or values, and
`StrategyConsentRequired` when execution is denied or consent is absent.
`RuntimeUnavailable` means no complete managed profile can be found;
`RuntimeIntegrityFailure` means a profile manifest or installed file fails
hash or identity validation. These failures retain source location metadata
when available and occur before Result-writer creation.

Each execution stores at most the diagnostic limits in §2.4. If the bound is
reached, later log output is drained and counted rather than allocating more
memory or blocking the worker. Validation errors identify a stable error code
and source line/column when available; runtime traceback uses the virtual
filename `strategy.py` and line numbers from the captured source, never an
absolute user path. Logs and tracebacks are untrusted text and are rendered as
text, not markup.

Debug Run and stepping are not part of the first milestone. They require a
separate execution-limit and persistence contract before implementation.

## 7. Required verification

- Every public condition, compiler operation, context member, worker message, hook, and command requires positive, negative, and boundary unit tests.
- Selectable Conditions require schema tests and golden tests against the C++ execution plan.
- Python requires lifecycle, transactional rollback, protocol violation, exception, timeout, cancellation, history/look-ahead, numeric-normalization, and runtime-version tests.
- Equivalent condition and Python strategies must produce identical canonical engine events when they express the same behavior.
- Every bug fix or intentional public-behavior change requires a regression test that fails against the old behavior.
- A check is merge-blocking only when [`CiDevFlow.md`](CiDevFlow.md) marks its implemented gate as required.

Lua, Zipline, native plugins, and AI authoring are unsupported.
