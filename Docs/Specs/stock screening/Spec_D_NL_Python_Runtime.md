# Spec D — NL / Python Runtime

**Part of:** Stock Screener sub-specs  
**UI reference:** [`Screener_UI_Overview.md`](./Screener_UI_Overview.md) §A4 (Python), §A5 (NL/AI)  
**DB schema:** [`Spec_C_Database.md`](./Spec_C_Database.md) §4.2, §4.5  
**Parent product spec:** [`11_Stock_Screener_KLine_Product.md`](../11_Stock_Screener_KLine_Product.md) §3  
**Coding conventions:** [`03_Backend_Core.md`](../03_Backend_Core.md) §1  
**Screener engine:** [`Spec_A_Screener_Engine.md`](./Spec_A_Screener_Engine.md)

---

## 1. Purpose

This spec covers the **two non-built-in authoring modes** of Block A:

- **Mode 2 — Python Script:** user writes a `screen()` function directly; C++ runs it in a sandbox.
- **Mode 3 — Natural Language (AI):** user types plain text; C++ sends it to Claude API, receives generated Python, user reviews and accepts before it runs.

This spec defines the **interface contracts** only.  
The choice of sandbox implementation (embedded CPython via `pybind11` vs subprocess IPC) is deferred to an ADR — this spec must not be invalidated by that choice.

---

## 2. Namespace & File Layout

```
Src/Backend/Scripting/
  BarsView.h              — read-only data structure passed into screen()
  IBarsSource.h           — pure virtual OHLCV+MA fetch interface
  DuckDbBarsSource.h / .cpp — MarketData.duckdb implementation of IBarsSource
  ScriptRequest.h         — engine input per symbol
  ScriptResult.h          — bool / float from screen()
  ISandboxRunner.h        — pure virtual Python execution interface
  INlBridge.h             — pure virtual NL/AI interface
  NlTypes.h               — NlRequest, NlResponse structs
  SandboxRunner_Subprocess.h / .cpp   — subprocess IPC implementation (Phase 1)
  SandboxRunner_CPython.h / .cpp      — embedded CPython (future, via ADR)
  NlBridge_Claude.h / .cpp            — Claude API implementation
  ScriptingController.h / .cpp        — orchestrates sandbox + NL bridge + audit
```

All types live in `namespace bte::scripting`.  
Conventions: same as `Spec_A §2`.

---

## 3. Python `screen()` API Contract

This is the **exact interface** a user's Python script must implement.  
The sandbox will reject scripts that do not conform to this signature.

### 3.1 Function Signature

```python
def screen(symbol: str, bars, as_of) -> bool | float:
    """
    Called once per symbol during a screen run.

    Parameters
    ----------
    symbol : str
        The ticker symbol being evaluated (e.g. "META").

    bars : pandas.DataFrame
        Historical data for `symbol` up to and including `as_of`.
        See §3.2 for the complete column list.
        The last row (bars.iloc[-1]) is the most recent bar.

    as_of : datetime.date
        The evaluation cutoff date (no-lookahead guarantee).
        bars contains NO rows with date > as_of.

    Returns
    -------
    bool
        True  → include this symbol in results
        False → exclude this symbol

    float
        A ranking score. Higher = ranked earlier.
        Returning a float implicitly means "include".

    Any other return type, or raising an exception → treated as False.
    """
```

**Float return → ranking rule:**  
When `screen()` returns a `float` for at least one symbol, `ScriptingController::runScript()` sorts the result set by float score descending. Symbols returning `True` (no float) are appended after float-scoring symbols, sorted by `marketCap` descending. `SymbolMatch::conditionsMet` is assigned the descending ordinal among float-returning symbols (`N` for top scorer, `N-1` for second, …, `1` for last); symbols returning `True` receive `conditionsMet = 0`.

### 3.2 `bars` DataFrame — Complete Column Reference

The `bars` DataFrame is built by joining `hourlyBars` and `fundamentals` for the given symbol up to `as_of`. All columns use `lowerCamelCase` matching the DB schema (Spec_C §5).

```
── Price & Volume (from hourlyBars, one row per trading day) ──────────────────
date          datetime64    bar date (index)
open          float64       opening price
high          float64       daily high
low           float64       daily low
close         float64       closing price
volume        int64         shares traded

── Moving Averages (computed from close, trailing N days) ─────────────────────
ma5           float64       5-day simple moving average
ma20          float64       20-day SMA
ma60          float64       60-day SMA
ma200         float64       200-day SMA

── Volume Ratio (computed) ────────────────────────────────────────────────────
volumeRatio   float64       volume / avg_volume_20d  (>2.0 = breakout volume)

── Fundamentals (from latest fundamentals row ≤ as_of, broadcast to all rows) ─
roe           float64       Return on Equity            (0.15 = 15%)
grossMargin   float64       Gross Margin                (0.30 = 30%)
debtEquity    float64       Debt / Equity ratio
fcf           float64       Free Cash Flow (USD absolute)
epsGrowth5Y   float64       5-year EPS CAGR             (0.18 = 18%)
pe            float64       P/E ratio (trailing)
pb            float64       P/B ratio
ps            float64       P/S ratio
peg           float64       PEG ratio
dividendYield float64       Annual dividend yield       (0.058 = 5.8%)
marketCap     float64       Market capitalisation (USD)

── Historical Averages (from fundamentals, broadcast) ─────────────────────────
peAvg5Y       float64       This symbol's own 5-year average P/E
peAvg10Y      float64       This symbol's own 10-year average P/E
yieldAvg5Y    float64       This symbol's own 5-year average yield
yieldAvg10Y   float64       This symbol's own 10-year average yield

── Cross-sectional Context (from fundamentals, broadcast) ─────────────────────
sectorAvgPe   float64       Sector median P/E on the as_of date
sectorAvgPb   float64       Sector median P/B on the as_of date
sectorMedianPs float64      Sector median P/S on the as_of date
sector        str           Sector name (e.g. "Information Technology")
```

**Key usage patterns:**

```python
# Latest value (most common)
roe = bars['roe'].iloc[-1]

# Check last 5 years of FCF all positive
fcf_positive = (bars['fcf'].tail(5) > 0).all()

# Momentum: 3-month price return
mom_3m = bars['close'].iloc[-1] / bars['close'].iloc[-63] - 1

# P/E below own historical average
pe_cheap = bars['pe'].iloc[-1] < bars['peAvg5Y'].iloc[-1]

# Return a ranking score (higher = better rank)
return roe * 10 + mom_3m * 5
```

### 3.3 Missing Values

Fundamentals columns may be `NaN` if the data pipeline has not yet populated them for that symbol. Scripts should guard:

```python
import math
if math.isnan(bars['roe'].iloc[-1]):
    return False
```

---

## 4. `BarsView` — C++ Representation

The `bars` DataFrame is constructed in C++ and handed to Python.  
Its C++ form is `BarsView` — a lightweight read-only structure the sandbox serialises as a pandas DataFrame.

### `BarsView.h`

```cpp
#pragma once
#include <string>
#include <vector>
#include <cmath>
#include "Core/Time.h"

namespace bte::scripting {

// One bar row — price/volume + joined fundamentals (broadcast per symbol).
struct BarRow {
    // Price & volume
    core::Timestamp date;
    double open   = std::numeric_limits<double>::quiet_NaN();
    double high   = std::numeric_limits<double>::quiet_NaN();
    double low    = std::numeric_limits<double>::quiet_NaN();
    double close  = std::numeric_limits<double>::quiet_NaN();
    int64_t volume = 0;   // integer — matches hourlyBars.volume BIGINT

    // Derived technicals
    double ma5         = std::numeric_limits<double>::quiet_NaN();
    double ma20        = std::numeric_limits<double>::quiet_NaN();
    double ma60        = std::numeric_limits<double>::quiet_NaN();
    double ma200       = std::numeric_limits<double>::quiet_NaN();
    double volumeRatio = std::numeric_limits<double>::quiet_NaN();

    // Fundamentals (same value on every row — broadcast from latest filing)
    double roe          = std::numeric_limits<double>::quiet_NaN();
    double grossMargin  = std::numeric_limits<double>::quiet_NaN();
    double debtEquity   = std::numeric_limits<double>::quiet_NaN();
    double fcf          = std::numeric_limits<double>::quiet_NaN();
    double epsGrowth5Y  = std::numeric_limits<double>::quiet_NaN();
    double pe           = std::numeric_limits<double>::quiet_NaN();
    double pb           = std::numeric_limits<double>::quiet_NaN();
    double ps           = std::numeric_limits<double>::quiet_NaN();
    double peg          = std::numeric_limits<double>::quiet_NaN();
    double dividendYield= std::numeric_limits<double>::quiet_NaN();
    double marketCap    = std::numeric_limits<double>::quiet_NaN();
    double peAvg5Y      = std::numeric_limits<double>::quiet_NaN();
    double peAvg10Y     = std::numeric_limits<double>::quiet_NaN();
    double yieldAvg5Y   = std::numeric_limits<double>::quiet_NaN();
    double yieldAvg10Y  = std::numeric_limits<double>::quiet_NaN();
    double sectorAvgPe  = std::numeric_limits<double>::quiet_NaN();
    double sectorAvgPb  = std::numeric_limits<double>::quiet_NaN();
    std::string sector;
};

// The full bars object passed to screen().
// Contains the last N bars up to asOfEnd (default: 252 trading days = ~1 year).
struct BarsView {
    std::string            symbol;
    std::vector<BarRow>    rows;      // chronological order, rows.back() = most recent
    core::Timestamp        asOfEnd;   // cutoff date — no rows beyond this
};

} // namespace bte::scripting
```

---

## 5. `IBarsSource` — OHLCV + Rolling MA Fetch Interface

### `IBarsSource.h`

```cpp
#pragma once
#include <vector>
#include "Core/Result.h"
#include "Core/Time.h"
#include "Scripting/BarsView.h"   // BarRow

namespace bte::scripting {

// Fetches OHLCV bars with pre-computed rolling MAs from MarketData.duckdb.
// Read-only (Spec 04 rule). Concrete implementation: DuckDbBarsSource.
// Fundamental fields in the returned BarRows are left NaN;
// ScriptingController::buildBarsView() broadcasts fundamentals into them.
class IBarsSource {
public:
    virtual ~IBarsSource() = default;

    // Returns up to numBars daily bars in chronological order (oldest first,
    // rows.back() = most recent bar) where ts <= asOfEnd.
    // Rolling MAs are computed over the full historical window so that the
    // last row's ma200 is always valid. Early rows may have NaN ma200 where
    // fewer than 200 prior bars exist in the DB.
    virtual core::Result<std::vector<BarRow>> fetchBars(
        std::string_view  symbol,
        core::Timestamp   asOfEnd,
        int               numBars = 252) const = 0;
};

} // namespace bte::scripting
```

### `DuckDbBarsSource.cpp` — Key SQL

```sql
-- Fetch price/volume with rolling MAs in one pass.
-- Outer query trims to numBars; the window functions see the full history
-- so that ma200 for the last row is computed from the prior 200 bars.
WITH bars AS (
    SELECT
        ts::DATE  AS date,
        open, high, low, close, volume,
        AVG(close) OVER (ORDER BY ts ROWS BETWEEN 4   PRECEDING AND CURRENT ROW) AS ma5,
        AVG(close) OVER (ORDER BY ts ROWS BETWEEN 19  PRECEDING AND CURRENT ROW) AS ma20,
        AVG(close) OVER (ORDER BY ts ROWS BETWEEN 59  PRECEDING AND CURRENT ROW) AS ma60,
        AVG(close) OVER (ORDER BY ts ROWS BETWEEN 199 PRECEDING AND CURRENT ROW) AS ma200,
        volume / NULLIF(
            AVG(volume) OVER (ORDER BY ts ROWS BETWEEN 19 PRECEDING AND CURRENT ROW),
            0
        ) AS volumeRatio,
        ROW_NUMBER() OVER (ORDER BY ts DESC) AS rn
    FROM hourlyBars
    WHERE symbol     = ?               -- parameterised: no SQL injection
      AND schemaName = 'ohlcv-1d'
      AND ts        <= ?               -- asOfEnd: no-lookahead
)
SELECT date, open, high, low, close, volume,
       ma5, ma20, ma60, ma200, volumeRatio
FROM bars
WHERE rn <= ?                          -- numBars (latest N rows)
ORDER BY date ASC;                     -- chronological: rows.back() = most recent
```

---

## 6. `ISandboxRunner` — Python Execution Interface

### `ISandboxRunner.h`

```cpp
#pragma once
#include <memory>
#include <string>
#include <variant>
#include "Core/Result.h"
#include "Scripting/BarsView.h"

namespace bte::scripting {

// Return value of screen(): bool (include/exclude) or float (rank score).
using ScriptReturn = std::variant<bool, double>;

// Input for one symbol evaluation.
struct ScriptRequest {
    std::string  scriptCode;  // full Python source of the screen() function
    std::string  symbol;
    BarsView     bars;        // data passed as the `bars` DataFrame
};

// Output for one symbol evaluation.
struct ScriptResult {
    std::string  symbol;
    ScriptReturn value;       // bool or float from screen()
    bool         timedOut   = false;
    bool         crashed    = false;
    std::string  errorMsg;    // non-empty if crashed
};

// Pure virtual interface — implementation chosen via ADR (subprocess or CPython).
class ISandboxRunner {
public:
    virtual ~ISandboxRunner() = default;

    // Creates a sandbox instance with the given resource limits.
    static core::Result<std::unique_ptr<ISandboxRunner>> create(
        int    timeoutMs  = 500,   // per-symbol execution budget
        size_t memLimitMb = 128    // max RSS per sandbox process
    );

    // Validates that scriptCode contains a valid screen() function.
    // Called on Validate button click — no symbol data needed.
    // Returns lint-style diagnostics as strings.
    virtual core::Result<std::vector<std::string>> validate(
        const std::string& scriptCode) const = 0;

    // Compiles and caches the script so run() calls are faster.
    // Called on Compile button click.
    virtual core::Result<void> compile(
        const std::string& scriptCode) = 0;

    // Runs screen() for one symbol. Non-blocking contract:
    // caller should run this in a thread pool, one task per symbol.
    virtual ScriptResult run(const ScriptRequest& req) const = 0;
};

} // namespace bte::scripting
```

---

## 6. Sandbox Rules

These rules apply to **all** Python scripts — both Mode 2 (user-written) and Mode 3 (NL-generated after Accept).

### 6.1 Forbidden Imports

Any attempt to import these modules causes the script to be **rejected at Validate time**:

```
os            sys           subprocess     socket
urllib        requests      http           ftplib
smtplib       paramiko      fabric         ansible
open          eval          exec           compile
__import__    importlib     ctypes         cffi
threading     multiprocessing  concurrent  asyncio
pathlib       glob          shutil         tempfile
pickle        shelve        sqlite3        psycopg2
```

The sandbox maintains an **allowlist** (everything not on the blocklist):

```
Allowed: math, statistics, numpy, pandas (read-only slice only),
         datetime, decimal, functools, itertools, collections,
         typing, dataclasses, enum, re
```

### 6.2 Resource Limits

| Limit | Value | Enforcement |
|---|---|---|
| Execution timeout | 500 ms per symbol | `SIGALRM` (subprocess) or `PyThreadState` interrupt (CPython) |
| Memory (RSS) | 128 MB per sandbox process | `setrlimit(RLIMIT_AS)` on Linux / Job Object on Windows |
| Output size | 1 KB | stdout/stderr captured and truncated |
| Disk access | Prohibited | `open()` patched to raise `PermissionError` |
| Network access | Prohibited | socket module removed from sandbox |

### 6.3 Determinism Requirement

Scripts **must not** produce different results given the same `(symbol, bars, as_of)` tuple.  
Randomness (`random`, `numpy.random`) is permitted in code but its use must be seeded with a fixed value, or results will fail CI determinism checks (Spec 10 §8).

---

## 7. `INlBridge` — Natural Language to Python Interface

### `NlTypes.h`

```cpp
#pragma once
#include <string>

namespace bte::scripting {

struct NlRequest {
    std::string  userPrompt;    // exact text the user typed
    std::string  modelId;       // e.g. "claude-sonnet-4-6"
    // Context injected by the system (not shown in UI, not editable)
    std::string  systemPrompt;  // see §7.2 for content
};

struct NlResponse {
    std::string  generatedCode; // extracted Python source
    std::string  modelId;       // echoed from request
    std::string  modelVersion;  // build/version string from API response
    std::string  sourceHash;    // sha256(generatedCode) — hex string
    bool         parseSuccess;  // false if no valid Python block found
    std::string  rawResponse;   // full API response (for debugging)
};

} // namespace bte::scripting
```

### `INlBridge.h`

```cpp
#pragma once
#include <memory>
#include "Core/Result.h"
#include "Scripting/NlTypes.h"

namespace bte::scripting {

// Pure virtual interface — concrete implementation: NlBridge_Claude.
class INlBridge {
public:
    virtual ~INlBridge() = default;

    static core::Result<std::unique_ptr<INlBridge>> create(
        const std::string& apiKey,
        const std::string& modelId = "claude-sonnet-4-6"
    );

    // Sends the user prompt to the AI model and returns generated Python.
    // Blocking — call from a worker thread.
    // Retries up to maxRetries times on parse failure.
    virtual core::Result<NlResponse> generate(
        const NlRequest& req,
        int maxRetries = 2) const = 0;
};

} // namespace bte::scripting
```

### 7.2 System Prompt (injected by C++)

The system prompt is **not shown to the user**. It constrains the model to produce valid, safe screener code.

```
You are a stock screener assistant. The user describes screening conditions in plain language.
Your job is to output a single Python function called `screen` with this exact signature:

    def screen(symbol: str, bars, as_of) -> bool | float:

Rules:
- Return True/float to include the symbol, False to exclude.
- `bars` is a pandas DataFrame. Available columns: [close, open, high, low, volume,
  ma5, ma20, ma60, ma200, volumeRatio, roe, grossMargin, debtEquity, fcf, epsGrowth5Y,
  pe, pb, ps, peg, dividendYield, marketCap, peAvg5Y, peAvg10Y, yieldAvg5Y, yieldAvg10Y,
  sectorAvgPe, sectorAvgPb, sectorMedianPs, sector]
- bars.iloc[-1] is the most recent row.
- Ratios are decimals (0.15 = 15%).
- Do NOT use: os, sys, open, requests, socket, eval, exec, subprocess, or any I/O.
- Output ONLY a ```python code block. No explanation outside the block.
```

### 7.3 Response Parsing

```cpp
// NlBridge_Claude.cpp — extract Python block from raw response
static std::optional<std::string>
extractPythonBlock(const std::string& rawResponse)
{
    // Find ```python ... ``` fence
    const std::string open  = "```python";
    const std::string close = "```";

    auto start = rawResponse.find(open);
    if (start == std::string::npos) return std::nullopt;
    start += open.size();

    // Skip leading newline
    if (start < rawResponse.size() && rawResponse[start] == '\n') ++start;

    auto end = rawResponse.find(close, start);
    if (end == std::string::npos) return std::nullopt;

    return rawResponse.substr(start, end - start);
}
```

If no valid block is found after `maxRetries` attempts, the bridge returns an error.  
The UI shows: *"Could not generate valid Python — please try rephrasing."*

---

## 8. `ScriptingController` — Orchestrates Both Modes

`ScriptingController` is the single entry point the Qt UI layer calls. It owns both the sandbox runner and the NL bridge.

### `ScriptingController.h`

```cpp
#pragma once
#include <memory>
#include <vector>
#include "Core/Result.h"
#include "Scripting/ISandboxRunner.h"
#include "Scripting/INlBridge.h"
#include "Scripting/IBarsSource.h"
#include "Screener/IFundamentalsRepository.h"
#include "Screener/ScreenerRequest.h"
#include "Screener/ScreenerResult.h"
#include "Scripting/AppDbAudit.h"   // audit write helpers (see §9)

namespace bte::scripting {

class ScriptingController {
public:
    static core::Result<std::unique_ptr<ScriptingController>> create(
        std::unique_ptr<ISandboxRunner>                          runner,
        std::unique_ptr<INlBridge>                               nlBridge,
        std::shared_ptr<bte::screener::IFundamentalsRepository>  repo,
        std::shared_ptr<IBarsSource>                             barsSource,  // hourlyBars+MA
        std::shared_ptr<AppDbAudit>                              audit
    );

    // ── Mode 2: Python Script ────────────────────────────────────────────────

    // Validates script syntax and sandbox compliance. No data loaded.
    core::Result<std::vector<std::string>> validateScript(
        const std::string& scriptCode) const;

    // Compiles the script (caches bytecode). Called on Compile button.
    core::Result<void> compileScript(const std::string& scriptCode);

    // Runs compiled script across all symbols. Called on Run Screen button.
    // Returns a ScreenerResult with symbols that returned true / highest float.
    // Float-return ranking: see §3.1 float return rule.
    core::Result<bte::screener::ScreenerResult> runScript(
        const std::string&                    scriptCode,
        const bte::screener::ScreenerRequest& req) const;

    // ── Mode 3: Natural Language ─────────────────────────────────────────────

    // Sends user prompt to AI, returns generated Python + audit metadata.
    // Inserts a pending row into nlAuditLog (accepted=0).
    core::Result<NlResponse> generateFromPrompt(
        const std::string& userPrompt,
        int*               outAuditId);   // set to the new nlAuditLog.id

    // Called when user clicks "Accept & Compile".
    // Updates nlAuditLog.accepted=1, saves screenerTemplate, links templateId.
    core::Result<int> acceptGeneratedCode(
        int            auditId,
        const QString& templateName);   // returns new screenerTemplates.id

    // Called when user clicks "Reject".
    // nlAuditLog row stays (accepted=0) — audit trail preserved.
    // No DB update needed; the row's accepted=0 already means rejected.
    void rejectGeneratedCode(int auditId);

private:
    // Joins IBarsSource (OHLCV + rolling MAs) with IFundamentalsRepository
    // (latest fundamentals row broadcast to all bar rows) to produce the
    // complete BarsView passed into screen(). See §5 for the SQL each uses.
    core::Result<BarsView> buildBarsView(
        std::string_view       symbol,
        const core::Timestamp& asOfEnd) const;

    std::unique_ptr<ISandboxRunner>                          runner_;
    std::unique_ptr<INlBridge>                               nlBridge_;
    std::shared_ptr<bte::screener::IFundamentalsRepository>  repo_;
    std::shared_ptr<IBarsSource>                             barsSource_;
    std::shared_ptr<AppDbAudit>                              audit_;
    std::string                                              compiledCode_;
};

} // namespace bte::scripting
```

### `ScriptingController.cpp` — `buildBarsView()` Implementation

```cpp
core::Result<BarsView>
ScriptingController::buildBarsView(
    std::string_view       symbol,
    const core::Timestamp& asOfEnd) const
{
    // 1. Fetch OHLCV + rolling MAs from hourlyBars (§5 SQL)
    auto barsRes = barsSource_->fetchBars(symbol, asOfEnd, /*numBars=*/252);
    if (!barsRes.ok()) return barsRes.error();

    BarsView view;
    view.symbol  = std::string(symbol);
    view.asOfEnd = asOfEnd;
    view.rows    = std::move(barsRes.value());

    // 2. Fetch latest fundamentals row (no-lookahead) — broadcast to all bar rows
    auto fundRes = repo_->latestBefore(symbol, asOfEnd);
    if (fundRes.ok()) {
        const bte::screener::FundamentalsRow& f = fundRes.value();
        for (auto& row : view.rows) {
            row.roe           = f.roe;
            row.grossMargin   = f.grossMargin;
            row.debtEquity    = f.debtEquity;
            row.fcf           = f.fcf;
            row.epsGrowth5Y   = f.epsGrowth5Y;
            row.pe            = f.pe;
            row.pb            = f.pb;
            row.ps            = f.ps;
            row.peg           = f.peg;
            row.dividendYield = f.dividendYield;
            row.marketCap     = f.marketCap;
            row.peAvg5Y       = f.peAvg5Y;
            row.peAvg10Y      = f.peAvg10Y;
            row.yieldAvg5Y    = f.yieldAvg5Y;
            row.yieldAvg10Y   = f.yieldAvg10Y;
            row.sectorAvgPe   = f.sectorAvgPe;
            row.sectorAvgPb   = f.sectorAvgPb;
            row.sectorMedianPs= f.sectorMedianPs;
        }
    }
    // If no fundamentals row exists: all fundamental fields remain NaN.
    // Python scripts must guard with math.isnan() — see §3.3.

    return view;
}
```

---

## 9. Audit Write Flow

### `AppDbAudit.h`

```cpp
#pragma once
#include <optional>
#include <string>
#include "Core/Result.h"

namespace bte::scripting {

// Thin wrapper over AppDb for NL audit operations.
// Defined separately to avoid coupling ScriptingController to the full AppDb.
class AppDbAudit {
public:
    // INSERT INTO nlAuditLog (prompt, modelId, modelVersion, sourceHash,
    //                         generatedCode, accepted=0)
    // Returns the new row id.
    virtual core::Result<int> insertPending(
        const std::string& prompt,
        const std::string& modelId,
        const std::string& modelVersion,
        const std::string& sourceHash,
        const std::string& generatedCode) = 0;

    // UPDATE nlAuditLog SET accepted=1, acceptedAt=now(), templateId=?
    virtual core::Result<void> markAccepted(
        int auditId, int templateId) = 0;

    // UPDATE nlAuditLog SET resultId=?
    // Called after Run Screen completes to complete the audit chain.
    virtual core::Result<void> linkResult(
        int auditId, int resultId) = 0;

    virtual ~AppDbAudit() = default;
};

} // namespace bte::scripting
```

### Audit Chain — Step by Step

```
User types prompt → clicks Send
│
├─► C++ calls INlBridge::generate(userPrompt)
│       → Claude API returns generatedCode
│
├─► INSERT nlAuditLog
│       prompt        = "Find profitable tech stocks..."
│       modelId       = "claude-sonnet-4-6"
│       modelVersion  = "build-2026-05"
│       sourceHash    = sha256(generatedCode)
│       generatedCode = "def screen(...):"
│       accepted      = 0        ← pending
│   → auditId = 7
│
├─► Show code in Chat bubble with [Accept] [Reject]
│
│   ── if REJECT ─────────────────────────────────────────────────────────────
│   No DB update. Row id=7 stays with accepted=0.
│   User types refined prompt → loop back to generate()
│
│   ── if ACCEPT ─────────────────────────────────────────────────────────────
│   ├─► UPDATE nlAuditLog SET accepted=1, acceptedAt=now() WHERE id=7
│   │
│   ├─► INSERT screenerTemplates
│   │       name       = "AI: profitable tech stocks"
│   │       mode       = "nl"
│   │       scriptCode = generatedCode
│   │       nlAuditId  = 7
│   │   → templateId = 12
│   │
│   ├─► UPDATE nlAuditLog SET templateId=12 WHERE id=7
│   │
│   └─► User clicks Run Screen
│           → ScriptingController::runScript(generatedCode, req)
│           → INSERT screenerResults  → resultId = 5
│           → UPDATE nlAuditLog SET resultId=5 WHERE id=7
│
│   Full audit chain complete:
│   nlAuditLog.id=7 ──► templateId=12 ──► screenerTemplates
│   nlAuditLog.id=7 ──► resultId=5   ──► screenerResults
```

---

## 10. How the Qt UI Calls ScriptingController

### Mode 2 — Python Script

![Python Script mode — code editor with Validate / Compile / Run Screen](./frontend%20-%20python%20script.PNG)

```cpp
// ScreenerWidget.cpp

// User clicks Validate
void ScreenerWidget::onValidateClicked()
{
    QString code = pythonEditor_->toPlainText();
    auto res = controller_->validateScript(code.toStdString());
    if (!res.ok()) { showError(res.error()); return; }

    lintPanel_->clear();
    for (const auto& msg : res.value())
        lintPanel_->addItem(QString::fromStdString(msg));
    lintPanel_->setVisible(true);
}

// User clicks Compile
void ScreenerWidget::onCompileClicked()
{
    auto res = controller_->compileScript(
        pythonEditor_->toPlainText().toStdString());
    statusLabel_->setText(res.ok() ? "Compiled. Ready to run." : "Compile failed.");
}

// User clicks Run Screen
void ScreenerWidget::onRunScreenClicked()
{
    bte::screener::ScreenerRequest req = buildRequest();
    QString code = pythonEditor_->toPlainText();

    auto* worker = new ScriptWorker(controller_, code.toStdString(), req, this);
    connect(worker, &ScriptWorker::finished,
            this,   &ScreenerWidget::onRunFinished);
    QThreadPool::globalInstance()->start(worker);
}
```

### Mode 3 — Natural Language

![Natural Language (AI) mode — chat UI with explicit Accept/Reject per response](./frontend%20-%20NL%20AI.PNG)

```cpp
// User clicks Send in NL chat
void ScreenerWidget::onNlSendClicked()
{
    QString prompt = nlPromptEdit_->toPlainText().trimmed();
    if (prompt.isEmpty()) return;

    int auditId = -1;
    auto res = controller_->generateFromPrompt(prompt.toStdString(), &auditId);
    if (!res.ok()) { addChatBubble(ChatRole::Error, res.error().message); return; }

    // Show generated code in AI bubble with Accept / Reject buttons
    addAiChatBubble(res.value().generatedCode, auditId);
}

// User clicks Accept in Chat bubble
void ScreenerWidget::onNlAcceptClicked(int auditId)
{
    QString name = QInputDialog::getText(
        this, "Name this script", "Template name:");
    if (name.isEmpty()) return;

    auto res = controller_->acceptGeneratedCode(auditId, name);
    if (!res.ok()) { showError(res.error()); return; }

    markBubbleAccepted(auditId);
    auditConfirmLabel_->setVisible(true);   // "Audit record saved."
}

// User clicks Reject
void ScreenerWidget::onNlRejectClicked(int auditId)
{
    controller_->rejectGeneratedCode(auditId);  // no-op in DB; row stays accepted=0
    markBubbleRejected(auditId);
}
```

---

## 11. Validate — Lint Output Format

`ISandboxRunner::validate()` returns a list of diagnostic strings shown in the Lint panel.

### Format

```
<severity> <code>  Line <n>: <message>
```

| Severity | Meaning |
|---|---|
| `OK` | No issue |
| `W` | Warning — script will still run |
| `E` | Error — script will NOT run until fixed |

### Example Output

```
OK    No syntax errors detected
OK    screen() signature is valid
W001  Line 3: 'epsGrowth5Y' could be NaN — consider adding isnan() guard
W002  Line 6: comparing float with == is unreliable; use >= or <=
E001  Line 9: forbidden import 'os' — not allowed in sandbox
```

If any `E` diagnostic exists, **Compile is blocked** and Run Screen is disabled.

---

## 12. Error Handling Reference

| Situation | Behaviour |
|---|---|
| Script contains forbidden import | `validate()` returns `E` diagnostic; compile blocked |
| `screen()` function missing from script | `validate()` returns `E001: screen() function not found` |
| `screen()` wrong signature | `validate()` returns `E002: signature must be screen(symbol, bars, as_of)` |
| Execution timeout (> 500 ms) | `ScriptResult.timedOut = true`; symbol excluded; warning shown |
| Script raises an exception | `ScriptResult.crashed = true`; `errorMsg` set; symbol excluded |
| Script returns wrong type | Treated as `False` (excluded); no error |
| Claude API unreachable | `INlBridge::generate()` returns `core::ErrorCode::timeout`; UI shows retry option |
| API returns no Python block after retries | UI shows: *"Could not parse response — try rephrasing"* |
| `nlAuditLog` INSERT fails | Error surfaced to user; Accept is blocked until DB is accessible |

---

## 13. Cross-References

| This spec section | References |
|---|---|
| §3.2 bars columns | `Spec_C §3.1` hourlyBars, `Spec_C §3.2` fundamentals |
| §4 BarsView construction | `Spec_A §8` DuckDbFundamentalsRepository queries |
| §6 Sandbox rules | `11_Stock_Screener_KLine_Product.md §3.1` safety posture |
| §9 Audit write flow | `Spec_C §4.5` nlAuditLog, `Spec_C §8.4` NL flow diagram |
| §9 Template save | `Spec_C §4.2` screenerTemplates, `Spec_A §7` template persistence |
| §9 Result link | `Spec_C §4.3` screenerResults |
| NL traceability | `11_Stock_Screener_KLine_Product.md §3.2` |
| Qt threading | `02_Frontend_Qt.md §3` — never block UI thread |
