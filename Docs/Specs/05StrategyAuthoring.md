# 05 — Strategy Authoring (Rules + Scripts + NL)

How users describe a trading strategy and how the backend turns that description into **buy / sell / hold** actions per bar.

We support **three product-facing authoring surfaces** (see also `11StockScreenerKLineProduct.md` §3):

1. **Built-in (rule) mode** — a JSON-backed declarative language editable from the Qt form. Covers ~80% of common strategies (crossovers, thresholds, breakouts, % stops). Multiple condition rows compose with **AND** or **OR** at the outermost level (`conditionLogic`: `all` | `any`; nested groups remain future work and are specified in `11` §2.2).
2. **Python script mode** — sandboxed Python implementing the same logical hook points as Lua (`§5`). This is the **primary code-first authoring path** described to end users (`00`, `02`, `11`).
3. **Natural language mode** — an AI-assisted flow that emits **candidate** Python or rule JSON which the user must **explicitly accept** before compile/run (`§6`).

**Lua mode** (`§4`) remains the reference **embedded interpreter** documented today; migrating or duplicating semantics for Python stays an implementation detail governed by ADRs until the Python host ships.

Rules, Lua, and Python targets all compile down to the same `IStrategy` C++ interface that the engine consumes (`07`).

---

## 1. The `IStrategy` interface

```cpp
namespace bte::strategy {

struct Context {
    core::Timestamp now;
    int barIndex;
    const core::Bar& bar;                            // the bar that just closed
    const Indicators& indicators;                    // see 06
    const PortfolioView& portfolio;                  // read-only view
    OrderBuilder& orders;                            // submit orders here
    spdlog::logger& log;
    StrategyConfig config;                           // user params
};

class IStrategy {
public:
    virtual ~IStrategy() = default;

    virtual core::Result<void> onInit(InitContext& ctx) = 0;
    virtual core::Result<void> onBar(Context& ctx) = 0;
    virtual core::Result<void> onFill(const core::Fill& fill, Context& ctx) {
        return {};   // default no-op
    }
    virtual core::Result<void> onShutdown() { return {}; }

    virtual std::string id() const = 0;
    virtual std::string version() const = 0;
};

}  // namespace bte::strategy
```

`InitContext` is similar to `Context` but called once before any bar arrives. It's where the strategy declares which indicators it needs (so the engine can pre-warm them):

```cpp
struct InitContext {
    IndicatorRegistry& registry;     // call registry.require("sma", {20})
    StrategyConfig& config;
    const core::DateRange& range;
    spdlog::logger& log;
};
```

---

## 2. `OrderBuilder`

The strategy never constructs `core::Order` directly. It speaks intent:

```cpp
class OrderBuilder {
public:
    void buy(double qty);                              // market buy
    void sell(double qty);                             // market sell

    void buyAtLimit(double qty, double limitPrice);
    void sellAtLimit(double qty, double limitPrice);

    void buyAtStop(double qty, double stopPrice);
    void sellAtStop(double qty, double stopPrice);

    void closeAll();                                   // flatten current symbol
    void cancelAll();                                  // cancel resting orders

    // sizing helpers
    void buyPctEquity(double pct);                     // buy N shares ≈ pct * equity / price
    void sellPctEquity(double pct);
    void buyShares(int shares);                        // explicit integer shares
};
```

The engine collects builds and produces `core::Order` instances with `createdAt = ctx.now`.

`OrderBuilder` is **per-bar**: cleared at the end of `onBar`. Strategies don't carry rolling order objects.

---

## 3. Rule mode

### 3.1 Schema (`*.rule.json`)

```json
{
  "id": "sma-cross-aapl",
  "name": "SMA 20/50 cross",
  "version": "1.0.0",
  "conditionLogic": "all",
  "universe": { "symbols": ["AAPL"], "schemaName": "ohlcv-1h" },
  "params": {
    "fast": 20,
    "slow": 50,
    "positionSizePctEquity": 0.10,
    "stopLossPct": 0.03
  },
  "indicators": [
    { "id": "fastSma", "kind": "sma", "args": { "period": "@fast" } },
    { "id": "slowSma", "kind": "sma", "args": { "period": "@slow" } },
    { "id": "rsi14",   "kind": "rsi", "args": { "period": 14 } }
  ],
  "rules": [
    {
      "name": "long entry",
      "when": [
        { "crosses": { "above": "fastSma", "below_now": "slowSma" } },
        { "rsi14": { "lt": 70 } },
        { "portfolio": { "noPositionIn": "AAPL" } }
      ],
      "do": [
        { "buyPctEquity": "@positionSizePctEquity" }
      ]
    },
    {
      "name": "long exit on cross-down",
      "when": [
        { "crosses": { "below": "fastSma", "above_now": "slowSma" } },
        { "portfolio": { "longIn": "AAPL" } }
      ],
      "do": [{ "closeAll": {} }]
    },
    {
      "name": "stop loss",
      "when": [
        { "portfolio": { "longIn": "AAPL" } },
        { "drawdownPctSinceEntry": { "gte": "@stopLossPct" } }
      ],
      "do": [{ "closeAll": {} }]
    }
  ]
}
```

#### Boolean composition (`conditionLogic`)

The top-level **`conditionLogic`** field controls how predicates inside each rule’s **`when`** array combine:

| `conditionLogic` | Meaning inside each `"when"` array |
| --- | --- |
| `"all"` (default when omitted) | **AND** — every predicate must be true for the rule’s `when` to pass. |
| `"any"` | **OR** — at least one predicate must be true. |

Complex nesting such as `(A AND B) OR (C AND D)` is **not** part of Phase-1 schema; duplicate rules or move logic into script modes until grouping lands (`11` §2.2). The **stock screener’s** flat AND/OR selector maps to this same discriminator for its predicate list.

### 3.2 Operators

| Operator | Args | Meaning |
|---|---|---|
| `crosses.above` | `{ above: <indicator|number>, below_now: <...> }` | true the bar `above` first exceeds `below_now` |
| `crosses.below` | mirror | dual |
| `gt`, `gte`, `lt`, `lte`, `eq` | number / indicator | scalar comparison on indicator value at current bar |
| `between` | `[lo, hi]` | inclusive |
| `slope` | `{ over: N, gt|lt: x }` | change in indicator over N bars |
| `portfolio.longIn` / `shortIn` / `noPositionIn` | symbol | portfolio query |
| `drawdownPctSinceEntry` | `gte: x` | open-position drawdown gate |
| `bar.close.gt` etc. | number / indicator | direct bar-field compares |

### 3.3 Actions

| Action | Args |
|---|---|
| `buy` | `{ qty: N }` |
| `sell` | `{ qty: N }` |
| `buyPctEquity` | number |
| `sellPctEquity` | number |
| `closeAll` | `{}` |
| `cancelAll` | `{}` |
| `log` | `{ level, message }` (templated, useful for debugging) |

### 3.4 Compilation

`bte::strategy::compileRule(json) -> Result<std::unique_ptr<IStrategy>>` walks the JSON, validates every reference (`fastSma`, `@fast`), pre-binds indicators against `InitContext`, and produces a `RuleStrategy` that evaluates each rule per bar using a small AST interpreter — fast enough that the per-bar overhead is < 1 µs.

The Qt rule-mode form generates and consumes this exact JSON; what you see is what's saved to disk.

---

## 4. Lua mode

### 4.1 Engine

We embed **Lua 5.4** (vendored, MIT) and use **sol2** (header-only, MIT) for binding.

Reasoning:
- Lua is small (~250 KB), embedding is one source dir, no build-system pain on any of the three OSes.
- sol2 gives us idiomatic C++ binding without the boilerplate of raw Lua C API.
- Lua 5.4 has `goto`, integer math, and bit ops — enough power without LuaJIT's complications.

LuaJIT is rejected: not available on Apple Silicon without workarounds, and we don't need its raw speed for per-bar decisions.

### 4.2 Sandbox

The Lua state is started with **only safe libs**: `string`, `table`, `math`, `bit32`. Removed: `os`, `io`, `package`, `debug.getregistry`, `loadfile`, `dofile`, `require`. Strategies cannot read or write the filesystem, can't open network sockets, can't `os.execute`.

A debug hook fires every 50,000 instructions and checks `std::stop_token` so cancelling a runaway script is instant.

### 4.3 The Lua API surface

```lua
-- declared globals available to every strategy script:

-- ctx.bar       table  { ts, open, high, low, close, volume }
-- ctx.barIndex  int
-- ctx.now       int    (unix millis, UTC)
-- ctx.portfolio table  read-only view
-- ctx.config    table  user params
-- bte           table  the API surface

local sma   = bte.indicator("sma",  { period = 20 })  -- in onInit
local slow  = bte.indicator("sma",  { period = 50 })

function onInit(ctx)
    ctx.config.fast = ctx.config.fast or 20
end

function onBar(ctx)
    local f = sma:value()
    local s = slow:value()
    if not f or not s then return end       -- still warming up

    if bte.crossesAbove(sma, slow) and ctx.portfolio:positionIn("AAPL") == 0 then
        bte.orders.buyPctEquity(ctx.config.positionSizePctEquity or 0.10)
    elseif bte.crossesBelow(sma, slow) and ctx.portfolio:positionIn("AAPL") > 0 then
        bte.orders.closeAll()
    end
end

function onFill(fill, ctx)
    bte.log.info(("filled %s %.2f x %d at %.2f"):format(
        fill.side, fill.qty, fill.orderId, fill.price))
end
```

### 4.4 Bound C++ surface (sol2)

```cpp
sol::table bte = lua["bte"].get_or_create<sol::table>();

bte["indicator"] = [&](std::string kind, sol::table args) {
    return registry.makeHandle(kind, args);
};
bte["crossesAbove"] = [&](IndicatorHandle a, IndicatorHandle b) {
    return engine.crossesAbove(a, b);
};
bte["orders"] = sol::table{};
bte["orders"]["buy"] = [&](double qty) { ctx.orders.buy(qty); };
// ...
bte["log"]["info"]  = [&](std::string m) { ctx.log.info(m); };

bte["apiVersion"] = std::string("1");   // major version of the Lua API
```

### 4.5 Compilation

```cpp
core::Result<std::unique_ptr<IStrategy>> compileLua(std::string_view source,
                                                    StrategyConfig defaults);
```

Errors during `lua_load` → `ErrorCode::strategyCompileFailed` with the Lua line/message. Runtime errors during `onBar` → `ErrorCode::strategyRuntimeError`; the engine logs and either stops the run (default) or skips the bar (configurable).

---

## 5. Python script mode (product requirement)

### 5.1 Role

Python is the **user-facing** code authoring language for strategies and for **custom screeners** (`11`). Compiled strategies still appear to the engine as `IStrategy`; how Python attaches (embedded interpreter, subprocess with IPC, or verified translator into Lua/IR) **must be decided in an ADR** before merging runtime code — this spec captures **behavioral expectations**.

### 5.2 API parity

Python strategies SHOULD expose lifecycle hooks analogous to Lua:

| Hook | Responsibility |
|---|---|
| `on_init(ctx)` | warm parameters, declare/prebind indicators (`InitContext`). |
| `on_bar(ctx)` | read portfolio + indicators, submit orders via `OrderBuilder` peer. |
| `on_fill(fill, ctx)` | optional journaling or state updates. |

The concrete module layout (`bte` package vs `globals`) mirrors whatever binding layer is chosen; **semantic parity with `§4.3`** is the contract.

### 5.3 Sandboxing & safety

Python must **not** gain ambient authority:

- deny or stub `open`, sockets, subprocess, `ctypes` to arbitrary native code, and import of non-allowlisted stdlib modules as determined by the ADR;
- honor `std::stop_token` cancellation for long-running user code;
- record `pythonApiVersion` alongside `bteApiVersion` in metadata once published.

### 5.4 Packaging on disk

| Artifact | Path |
|---|---|
| Source | `<userData>/strategies/<id>.py` |
| Metadata | `<userData>/strategies/<id>.meta.json` (NL prompt hash, accepted model id, etc.) |

### 5.5 Compilation entry point (name TBD)

```cpp
core::Result<std::unique_ptr<IStrategy>> compilePython(std::string_view source,
                                                       StrategyConfig defaults);
```

Errors map to the same `ErrorCode` family as Lua (`strategyCompileFailed`, `strategyRuntimeError`).

---

## 6. Natural language (AI-assisted) authoring

### 6.1 Flow

1. User enters a **natural language** description of entries, exits, sizing, or filters.
2. An **agent** (local model, cloud API, or hybrid — out of scope here) returns **structured output**: either candidate **Python** source or **rule JSON**, never silent execution.
3. UI shows a **diff/preview**; user clicks **Accept** to copy the artifact into the Python or rule editor, then runs the normal `compile*` path.
4. Persist `nlPrompt.txt` (or embedded field inside `.meta.json`) + `acceptedSourceSha256` for audit.

### 6.2 Guardrails

- No auto-run of AI output without explicit user confirmation **and** successful compile (`11` §3.2).
- Treat model output as **untrusted code** until validated by the sandbox + compiler.
- Determinism: LLM sampling must be pinned or temperature=0 when generating executable strategy text used for regression fixtures.

---

## 7. Strategy persistence & versioning

| Field | Where stored | Notes |
|---|---|---|
| `id` | inside file | unique within `<userData>/strategies/` |
| `version` | inside file | semver; bumped manually by user |
| `bteApiVersion` | inside file | the `bte.apiVersion` it targets; rejected if app's major != file's major |
| `pythonApiVersion` | inside `.meta.json` or PEP 723 header (TBD) | present for **`*.py`** once Python host ships; analogous rules to Lua major mismatch |
| `createdAt`, `updatedAt` | file mtime | also stored inside for portability |
| Source format | `*.rule.json`, `*.lua`, or `*.py` + sibling `*.meta.json` | the `.meta.json` carries non-source fields for scripted strategies + NL traces |

The Strategies tab UI lists all files, filterable by tag.

---

## 8. Live validation in the editor

While the user types:
- **Rule mode**: each form change re-renders JSON, calls `compileRule`, displays errors inline (`QLineEdit::setStyleSheet("background: #fee")`).
- **Lua mode**: 500 ms debounce → `compileLua` (only `lua_load`, doesn't run). Errors highlight the offending line via the syntax highlighter.
- **Python mode**: same debounce pattern → `compilePython` parse/static checks; optional `ruff`/`basedpyright` hooks when available.
- **NL mode**: validate only **after** user accepts generated text into one of the concrete editors.

All concrete code modes show **indicator preview**: pick a symbol, press "Preview", and a thumbnail chart shows the indicator overlaid on recent bars without running a full backtest. Implemented by feeding 500 historical bars through `Indicators` only.

---

## 9. Determinism

Every strategy run with the same `(strategy file, data range, params, engine config)` must produce **byte-identical** trades. Script randomness is seeded deterministically (`math.random` in Lua today; Python’s `random` module must expose the same guarantee once enabled). Users may override deliberately:

```lua
math.randomseed(123)   -- once in onInit (Lua)
```

```python
random.seed(123)       # once in on_init (Python; illustrative)
```

The engine's broker simulator is deterministic by construction (`07`).

---

## 10. Tests

- Rule compiler: every operator round-trips JSON → AST → JSON.
- Lua sandbox escape attempts (load `os`, write a file): all fail.
- A reference strategy (`sma-cross-aapl`) implemented in **both** rule and Lua produces identical trade lists on a fixed dataset — locks in semantic equivalence.
- **Python (when shipped)**: the same fixture must match rule/Lua trades or document intentional deltas with regression tests proving equivalence.
- **NL harness (offline)**: golden-file tests proving accepted artifacts compile; no network in CI unless using recorded fixtures.
- Cancellation: an infinite-loop Lua script terminates within 50 ms of `stopSource_.request_stop()`.
