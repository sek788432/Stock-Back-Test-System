# Spec A — Screener Engine

**Part of:** Stock Screener sub-specs  
**UI reference:** [`Screener_UI_Overview.md`](./Screener_UI_Overview.md) §A1–A6  
**DB schema:** [`Spec_C_Database.md`](./Spec_C_Database.md)  
**Parent product spec:** [`11_Stock_Screener_KLine_Product.md`](../11_Stock_Screener_KLine_Product.md) §2  
**Coding conventions:** [`03_Backend_Core.md`](../03_Backend_Core.md) §1  
**Referenced by:** `Spec_B_Valuation_Engine.md`, `Spec_D_NL_Python_Runtime.md`

---

## 1. Purpose

This spec defines the **logic layer** of Block A (Screener) as C++ code:

- Header files with full type definitions
- Interface contracts for DB access
- Concrete SQL patterns against `MarketData.duckdb` and `app.db`
- How to call the engine from the Qt UI layer

UI behaviour is in `Screener_UI_Overview.md`.  
DB schema (column types, indexes) is in `Spec_C_Database.md`.  
Python / NL execution is in `Spec_D_NL_Python_Runtime.md`.

---

## 2. Namespace & File Layout

```
Src/Backend/Screener/
  ConditionBlock.h          — atomic condition struct + enums
  FundamentalsRow.h         — in-memory snapshot of one fundamentals DB row
  ScreenerRequest.h         — engine input
  ScreenerResult.h          — engine output
  IFundamentalsRepository.h — pure virtual DB access interface
  IScreenerEngine.h         — pure virtual engine interface
  DuckDbFundamentalsRepository.h / .cpp  — MarketData.duckdb implementation
  ScreenerEngine.h / .cpp              — engine implementation
  AppDb.h / .cpp            — app.db SQLite operations (Qt)
```

All types live in `namespace bte::screener`.  
Conventions from `03_Backend_Core.md §1`:
- Types → `UpperCamelCase`
- Variables / methods → `lowerCamelCase`
- Private members → trailing underscore (`conn_`)
- Enum values → scoped `lowerCamelCase` (`ScreenerLogic::andAll`)
- `#pragma once` on every header
- No raw `new` / `delete` — RAII throughout
- No exceptions across module boundaries — use `core::Result<T>`

---

## 3. Enums

### `ConditionBlock.h` — enums section

```cpp
#pragma once
#include <string>
#include <variant>
#include <vector>
#include "Core/Result.h"   // bte::core::Result

namespace bte::screener {

// ── Logic ───────────────────────────────────────────────────────────────────

enum class ScreenerLogic {
    andAll,   // ALL conditions must pass (AND)
    orAny     // ANY one condition passes  (OR)
};

// ── Operators ───────────────────────────────────────────────────────────────

enum class ConditionOperator {
    // Numeric
    gte,                  // field >= value
    lte,                  // field <= value
    gt,                   // field >  value
    lt,                   // field <  value
    eq,                   // field == value  (numeric or enum string)
    between,              // low <= field <= high
    in,                   // field is one of [v1, v2, ...]
    // Special multi-bar
    trend,                // EPS direction: "stable" | "growing" | "declining"
    consecutivePositive,  // field > 0 for last N periods
    ratioVsAvg,           // pe < peAvg5Y AND pe < peAvg10Y
    yieldAboveAvg         // dividendYield > yieldAvg5Y AND dividendYield > 0
};

// ── Value display hints (UI only, does not affect evaluation) ────────────────

enum class ValueType {
    decimal,    // 0.15 = 15%  — displayed as percentage
    absolute,   // raw number  — displayed as-is
    enumValue,  // string enum — displayed verbatim
    boolean
};

// ── Derived technical enums ──────────────────────────────────────────────────

enum class MaAlignment {
    bullish,      // ma5 > ma20 > ma60 > ma200
    partialBull,  // ma5 > ma20 > ma60 (ma200 not required)
    neutral,
    partialBear,  // ma5 < ma20 < ma60
    bearish       // ma5 < ma20 < ma60 < ma200
};

enum class PriceVsMa {
    above,
    below
};

enum class TrendDirection {
    stable,    // at most 1 EPS dip in 5Y
    growing,   // all EPS increasing
    declining  // 3+ consecutive dips
};

} // namespace bte::screener
```

---

## 4. `ConditionBlock` — Atomic Condition Unit

### `ConditionBlock.h` (continued)

```cpp
// ── Condition value: holds whatever the operator needs ──────────────────────

struct ConditionValue {
    // Exactly one of the following is meaningful, depending on operator:
    //   single double  → gte / lte / gt / lt / eq / consecutivePositive
    //   string         → eq / trend (enum target)
    //   pair<d,d>      → between [low, high]
    //   vector<string> → in [v1, v2, ...]
    std::variant<
        double,
        std::string,
        std::pair<double, double>,
        std::vector<std::string>
    > data;

    // Convenience constructors
    explicit ConditionValue(double v)                       : data(v) {}
    explicit ConditionValue(std::string s)                  : data(std::move(s)) {}
    explicit ConditionValue(double lo, double hi)           : data(std::pair{lo, hi}) {}
    explicit ConditionValue(std::vector<std::string> vs)    : data(std::move(vs)) {}

    double          asDouble()  const { return std::get<double>(data); }
    std::string_view asString() const { return std::get<std::string>(data); }
    std::pair<double,double> asRange() const { return std::get<std::pair<double,double>>(data); }
    const std::vector<std::string>& asList() const { return std::get<std::vector<std::string>>(data); }
};

// ── The atomic condition ─────────────────────────────────────────────────────

struct ConditionBlock {
    std::string      category;   // display grouping ("Moat", "Technical") — cosmetic only
    std::string      field;      // key into FieldRegistry (see §7)
    ConditionOperator op;
    ConditionValue   value;
    ValueType        valueType;  // display hint for the UI shelf

    // Serialise to JSON string for storage in screenerTemplates.conditions
    std::string toJson() const;

    // Deserialise from a nlohmann::json object
    static core::Result<ConditionBlock> fromJson(const nlohmann::json& j);
};
```

### JSON wire format (stored in `screenerTemplates.conditions`)

```json
{
    "category":  "Moat",
    "field":     "roe",
    "op":        "gte",
    "value":     0.15,
    "valueType": "decimal"
}
```

For `between`: `"value": [10.0, 25.0]`  
For `in`: `"value": ["Information Technology", "Health Care"]`

---

## 5. `FundamentalsRow` — In-Memory DB Snapshot

### `FundamentalsRow.h`

```cpp
#pragma once
#include <optional>
#include <string>
#include "Core/Time.h"       // bte::core::Timestamp
#include "Screener/ConditionBlock.h"

namespace bte::screener {

// One row of fundamentals data for one symbol at one point in time.
// Populated by IFundamentalsRepository::latestBefore().
// NaN (std::numeric_limits<double>::quiet_NaN()) signals a missing value.
// Callers MUST check isNaN() before using any double field.

struct FundamentalsRow {
    // ── Identity ────────────────────────────────────────────────────────────
    std::string          symbol;
    core::Timestamp      asOfDate;   // the filing / data date (not the query date)

    // ── Quality metrics (Spec_C §3.2) ───────────────────────────────────────
    double roe          = NaN;   // Return on Equity           (0.15 = 15%)
    double grossMargin  = NaN;   // Gross Profit / Revenue     (0.30 = 30%)
    double debtEquity   = NaN;   // Total Debt / Equity        (0.50 = 50%)
    double fcf          = NaN;   // Free Cash Flow             (absolute USD)
    double epsGrowth5Y  = NaN;   // EPS CAGR over 5 years

    // ── Valuation ratios ────────────────────────────────────────────────────
    double pe           = NaN;
    double pb           = NaN;
    double ps           = NaN;
    double peg          = NaN;
    double dividendYield= NaN;   // annual yield (0.058 = 5.8%)
    double marketCap    = NaN;   // USD

    // ── Historical averages (Spec_C §3.2, new columns) ──────────────────────
    double peAvg5Y      = NaN;
    double peAvg10Y     = NaN;
    double yieldAvg5Y   = NaN;
    double yieldAvg10Y  = NaN;

    // ── Cross-sectional context ──────────────────────────────────────────────
    double sectorAvgPe    = NaN;
    double sectorAvgPb    = NaN;
    double sectorMedianPs = NaN;   // required by Spec_D §3.2 bars DataFrame
    std::string sector;

    // ── Technical fields (populated only when hourlyBars are loaded) ─────────
    std::optional<double>      lastPrice;
    std::optional<double>      changePercent;   // vs previous close
    std::optional<double>      volumeRatio;     // volume / 20d avg volume
    std::optional<PriceVsMa>   priceVsMa200;
    std::optional<PriceVsMa>   priceVsMa20;
    std::optional<MaAlignment> maAlignment;

    // ── Helpers ─────────────────────────────────────────────────────────────
    static constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
    static bool isNaN(double v) noexcept { return std::isnan(v); }
};

} // namespace bte::screener
```

---

## 6. `ScreenerRequest` & `ScreenerResult`

### `ScreenerRequest.h`

```cpp
#pragma once
#include <string>
#include <vector>
#include "Core/Time.h"
#include "Screener/ConditionBlock.h"

namespace bte::screener {

struct ScreenerRequest {
    // ── Universe ─────────────────────────────────────────────────────────────
    std::string              universe;   // "SP500" | "NASDAQ100" | "NYSE" | "CUSTOM"
    std::vector<std::string> customSymbols; // only used when universe == "CUSTOM"

    // ── Time window ──────────────────────────────────────────────────────────
    core::Timestamp asOfEnd;     // no-lookahead cutoff: only data <= this date used
    core::Timestamp asOfStart;   // stored for audit purposes only, not used in eval

    // ── Conditions ───────────────────────────────────────────────────────────
    ScreenerLogic                logic;
    std::vector<ConditionBlock>  conditions;
};

} // namespace bte::screener
```

### `ScreenerResult.h`

```cpp
#pragma once
#include <string>
#include <vector>
#include "Core/Time.h"

namespace bte::screener {

struct SymbolMatch {
    std::string symbol;
    std::string name;           // from stocks table
    std::string sector;
    double      lastPrice      = 0.0;
    double      changePercent  = 0.0;
    double      marketCap      = 0.0;
    int         conditionsMet  = 0;  // used for ranking (descending)
};

struct ScreenerResult {
    std::vector<SymbolMatch>  matches;         // sorted by rank (see §9)
    core::Timestamp           evaluatedAt;
    int                       totalEvaluated = 0;   // symbols checked
    std::vector<std::string>  warnings;             // e.g. missing data notices
};

} // namespace bte::screener
```

---

## 7. `IFundamentalsRepository` — DB Access Interface

### `IFundamentalsRepository.h`

```cpp
#pragma once
#include <optional>
#include <string>
#include <vector>
#include "Core/Result.h"
#include "Core/Time.h"
#include "Screener/FundamentalsRow.h"

namespace bte::screener {

// Pure virtual interface for fetching fundamentals from MarketData.duckdb.
// Concrete implementation: DuckDbFundamentalsRepository.
// Test double: MockFundamentalsRepository.

class IFundamentalsRepository {
public:
    virtual ~IFundamentalsRepository() = default;

    // Returns the latest fundamentals row where asOfDate <= cutoff.
    // Returns core::Error::notFound if no row exists for this symbol before cutoff.
    virtual core::Result<FundamentalsRow> latestBefore(
        std::string_view    symbol,
        core::Timestamp     cutoff) const = 0;

    // Returns the last maxRows rows before cutoff, newest first.
    // Used by trend and consecutivePositive operators.
    virtual core::Result<std::vector<FundamentalsRow>> historyBefore(
        std::string_view    symbol,
        core::Timestamp     cutoff,
        int                 maxRows) const = 0;

    // Returns all symbols in the given universe (e.g. "SP500").
    // Populated from the stocks / index-constituent table in MarketData.duckdb.
    virtual core::Result<std::vector<std::string>> symbolsInUniverse(
        std::string_view universe) const = 0;
};

} // namespace bte::screener
```

---

## 8. `DuckDbFundamentalsRepository` — Concrete Implementation

### `DuckDbFundamentalsRepository.h`

```cpp
#pragma once
#include <filesystem>
#include "Screener/IFundamentalsRepository.h"
#include "duckdb.hpp"   // vendored in ThirdParty/duckdb/

namespace bte::screener {

class DuckDbFundamentalsRepository final : public IFundamentalsRepository {
public:
    // Opens MarketData.duckdb in read-only mode (Spec 04 hard rule).
    // Returns error if the file is missing, locked, or schema mismatches.
    static core::Result<std::unique_ptr<DuckDbFundamentalsRepository>>
        open(const std::filesystem::path& duckDbPath);

    ~DuckDbFundamentalsRepository() override;

    core::Result<FundamentalsRow> latestBefore(
        std::string_view symbol, core::Timestamp cutoff) const override;

    core::Result<std::vector<FundamentalsRow>> historyBefore(
        std::string_view symbol, core::Timestamp cutoff, int maxRows) const override;

    core::Result<std::vector<std::string>> symbolsInUniverse(
        std::string_view universe) const override;

private:
    explicit DuckDbFundamentalsRepository(
        duckdb::DuckDB db, duckdb::Connection conn);

    duckdb::DuckDB       db_;
    duckdb::Connection   conn_;
};

} // namespace bte::screener
```

### `DuckDbFundamentalsRepository.cpp` — Key SQL Patterns

#### Opening the connection (read-only)

```cpp
core::Result<std::unique_ptr<DuckDbFundamentalsRepository>>
DuckDbFundamentalsRepository::open(const std::filesystem::path& path)
{
    duckdb::DBConfig cfg;
    cfg.options.access_mode = duckdb::AccessMode::READ_ONLY;  // Spec 04

    duckdb::DuckDB db(path.string(), &cfg);
    duckdb::Connection conn(db);

    // Verify required columns exist (follows Spec 04 §3.1 schema discovery)
    auto check = conn.Query("PRAGMA table_info('fundamentals')");
    if (check->HasError())
        return core::Error{core::ErrorCode::schemaMismatch,
            "fundamentals table not found — run Python pipeline first"};

    return std::unique_ptr<DuckDbFundamentalsRepository>(
        new DuckDbFundamentalsRepository(std::move(db), std::move(conn)));
}
```

#### `latestBefore` — no-lookahead point-in-time query

```cpp
core::Result<FundamentalsRow>
DuckDbFundamentalsRepository::latestBefore(
    std::string_view symbol, core::Timestamp cutoff) const
{
    // SQL: one row per symbol, newest asOfDate that is still <= cutoff
    auto res = conn_.Query(R"(
        SELECT
            symbol, asOfDate,
            roe, grossMargin, debtEquity, fcf, epsGrowth5Y,
            pe, pb, ps, peg, dividendYield, marketCap,
            peAvg5Y, peAvg10Y, yieldAvg5Y, yieldAvg10Y,
            sectorAvgPe, sectorAvgPb, sectorMedianPs, sector
        FROM fundamentals
        WHERE symbol  = ?
          AND asOfDate <= ?                 -- ← no-lookahead guard
        ORDER BY asOfDate DESC
        LIMIT 1
    )", symbol, core::toIso8601(cutoff));    // parameterised — no SQL injection

    if (res->RowCount() == 0)
        return core::Error{core::ErrorCode::notFound,
            std::string("no fundamentals for ") + std::string(symbol)};

    FundamentalsRow row;
    auto chunk = res->Fetch();
    row.symbol      = chunk->GetValue<std::string>(0, 0);
    row.roe         = chunk->GetValue<double>(2, 0);
    row.grossMargin = chunk->GetValue<double>(3, 0);
    // ... (map remaining columns in declaration order)
    return row;
}
```

#### `historyBefore` — multi-row look-back for trend / consecutive operators

```cpp
core::Result<std::vector<FundamentalsRow>>
DuckDbFundamentalsRepository::historyBefore(
    std::string_view symbol, core::Timestamp cutoff, int maxRows) const
{
    // Fetch all fields used by trend/consecutivePositive operators via FieldRegistry
    auto res = conn_.Query(R"(
        SELECT asOfDate,
               roe, grossMargin, debtEquity, fcf, epsGrowth5Y,
               pe, pb, ps, dividendYield, marketCap
        FROM fundamentals
        WHERE symbol  = ?
          AND asOfDate <= ?
        ORDER BY asOfDate DESC
        LIMIT ?
    )", symbol, core::toIso8601(cutoff), maxRows);

    std::vector<FundamentalsRow> rows;
    while (auto chunk = res->Fetch()) {
        for (idx_t r = 0; r < chunk->size(); ++r) {
            FundamentalsRow row;
            row.symbol      = std::string(symbol);
            row.roe         = chunk->GetValue<double>(1, r);
            row.grossMargin = chunk->GetValue<double>(2, r);
            row.debtEquity  = chunk->GetValue<double>(3, r);
            row.fcf         = chunk->GetValue<double>(4, r);
            row.epsGrowth5Y = chunk->GetValue<double>(5, r);
            row.pe          = chunk->GetValue<double>(6, r);
            row.pb          = chunk->GetValue<double>(7, r);
            row.ps          = chunk->GetValue<double>(8, r);
            row.dividendYield = chunk->GetValue<double>(9, r);
            row.marketCap   = chunk->GetValue<double>(10, r);
            rows.push_back(row);
        }
    }
    return rows;
}
```

#### `symbolsInUniverse` — universe expansion

```cpp
core::Result<std::vector<std::string>>
DuckDbFundamentalsRepository::symbolsInUniverse(std::string_view universe) const
{
    // Phase 1: returns all symbols with fundamentals data; `universe` param is ignored.
    // Phase 2: add indexConstituents table (symbol, indexName, effectiveDate) written by
    // Python pipeline — see API_Data_Requirements.md §2.3. Then filter:
    //   WHERE indexName = universe AND effectiveDate <= cutoff AND removed = 0
    (void)universe;
    auto res = conn_.Query(R"(
        SELECT DISTINCT symbol
        FROM fundamentals
        WHERE sector IS NOT NULL
        ORDER BY symbol
    )");

    std::vector<std::string> syms;
    while (auto chunk = res->Fetch())
        for (idx_t r = 0; r < chunk->size(); ++r)
            syms.push_back(chunk->GetValue<std::string>(0, r));
    return syms;
}
```

---

## 9. `IScreenerEngine` — Engine Interface

### `IScreenerEngine.h`

```cpp
#pragma once
#include <memory>
#include "Core/Result.h"
#include "Screener/IFundamentalsRepository.h"
#include "Screener/ScreenerRequest.h"
#include "Screener/ScreenerResult.h"

namespace bte::screener {

class IScreenerEngine {
public:
    virtual ~IScreenerEngine() = default;

    static core::Result<std::unique_ptr<IScreenerEngine>> create(
        std::shared_ptr<IFundamentalsRepository> repo);

    // Evaluate all conditions against every symbol in the universe.
    // Blocking — call from a worker thread, not the Qt UI thread.
    virtual core::Result<ScreenerResult> run(
        const ScreenerRequest& req) const = 0;
};

} // namespace bte::screener
```

### `ScreenerEngine.cpp` — AND / OR Evaluation Loop

```cpp
core::Result<ScreenerResult>
ScreenerEngine::run(const ScreenerRequest& req) const
{
    ScreenerResult result;
    result.evaluatedAt = core::now();

    // 1. Expand universe → list of symbols
    auto symsRes = repo_->symbolsInUniverse(req.universe);
    if (!symsRes.ok()) return symsRes.error();
    const auto& symbols = symsRes.value();
    result.totalEvaluated = static_cast<int>(symbols.size());

    // 2. Evaluate each symbol
    for (const auto& sym : symbols) {
        auto rowRes = repo_->latestBefore(sym, req.asOfEnd);
        if (!rowRes.ok()) {
            // No data → exclude + warn
            result.warnings.push_back("no data for " + sym);
            continue;
        }
        const FundamentalsRow& row = rowRes.value();

        // 3. Evaluate all conditions for this symbol
        int met = 0;
        bool include = (req.logic == ScreenerLogic::andAll);  // AND starts true, OR starts false

        for (const auto& cond : req.conditions) {
            bool pass = evaluateCondition(cond, row, req.asOfEnd);
            if (pass) ++met;

            if (req.logic == ScreenerLogic::andAll) {
                if (!pass) { include = false; break; }  // AND short-circuit
            } else {
                if (pass) { include = true;  break; }   // OR  short-circuit
            }
        }

        if (include) {
            SymbolMatch m;
            m.symbol        = sym;
            m.marketCap     = row.marketCap;
            m.conditionsMet = met;
            // name + sector populated from stocks join (omitted for brevity)
            result.matches.push_back(m);
        }
    }

    // 4. Sort: conditionsMet DESC, marketCap DESC, symbol ASC
    std::sort(result.matches.begin(), result.matches.end(),
        [](const SymbolMatch& a, const SymbolMatch& b) {
            if (a.conditionsMet != b.conditionsMet)
                return a.conditionsMet > b.conditionsMet;
            if (a.marketCap != b.marketCap)
                return a.marketCap > b.marketCap;
            return a.symbol < b.symbol;
        });

    return result;
}
```

### `evaluateCondition` — Single Condition Dispatch

```cpp
bool ScreenerEngine::evaluateCondition(
    const ConditionBlock&  cond,
    const FundamentalsRow& row,
    core::Timestamp        cutoff) const
{
    // Resolve field value from the row
    auto fieldVal = FieldRegistry::resolve(cond.field, row);
    if (!fieldVal.has_value()) return false;  // missing data → condition fails

    switch (cond.op) {
    case ConditionOperator::gte: return fieldVal->asDouble() >= cond.value.asDouble();
    case ConditionOperator::lte: return fieldVal->asDouble() <= cond.value.asDouble();
    case ConditionOperator::gt:  return fieldVal->asDouble() >  cond.value.asDouble();
    case ConditionOperator::lt:  return fieldVal->asDouble() <  cond.value.asDouble();
    case ConditionOperator::eq:
        if (std::holds_alternative<double>(cond.value.data))
            return fieldVal->asDouble() == cond.value.asDouble();
        return fieldVal->asString() == cond.value.asString();
    case ConditionOperator::between: {
        auto [lo, hi] = cond.value.asRange();
        return lo <= fieldVal->asDouble() && fieldVal->asDouble() <= hi;
    }
    case ConditionOperator::in: {
        auto str = std::string(fieldVal->asString());
        const auto& list = cond.value.asList();
        return std::find(list.begin(), list.end(), str) != list.end();
    }
    case ConditionOperator::consecutivePositive:
        return checkConsecutivePositive(row.symbol, cond, cutoff);
    case ConditionOperator::trend:
        return checkTrend(row.symbol, cond, cutoff);
    case ConditionOperator::ratioVsAvg:
        return !FundamentalsRow::isNaN(row.pe)
            && !FundamentalsRow::isNaN(row.peAvg5Y)
            && !FundamentalsRow::isNaN(row.peAvg10Y)
            && row.pe < row.peAvg5Y
            && row.pe < row.peAvg10Y;
    case ConditionOperator::yieldAboveAvg:
        return !FundamentalsRow::isNaN(row.dividendYield)
            && !FundamentalsRow::isNaN(row.yieldAvg5Y)
            && row.dividendYield > row.yieldAvg5Y
            && row.dividendYield > 0.0;
    }
    return false;
}
```

---

### `checkConsecutivePositive` — N-Period Positivity Check

Checks that a given field has been > 0 for the last N quarterly rows.  
`cond.value` stores N as a double. Uses `repo_->historyBefore()` which is a member of `ScreenerEngine`.

```cpp
bool ScreenerEngine::checkConsecutivePositive(
    std::string_view      symbol,
    const ConditionBlock& cond,
    core::Timestamp       cutoff) const
{
    int n = static_cast<int>(cond.value.asDouble());
    if (n <= 0) return false;

    auto histRes = repo_->historyBefore(symbol, cutoff, n);
    if (!histRes.ok() || static_cast<int>(histRes.value().size()) < n)
        return false;  // not enough history → condition fails

    for (const auto& row : histRes.value()) {
        auto fv = FieldRegistry::resolve(cond.field, row);
        if (!fv.has_value() || fv->asDouble() <= 0.0) return false;
    }
    return true;
}
```

**Example:** `field="fcf", op=consecutivePositive, value=5` → FCF > 0 for each of the last 5 quarters.

---

### `checkTrend` — Directional Trend Over 5 Periods

`cond.value` holds the target direction string: `"growing"` | `"stable"` | `"declining"`.

```cpp
bool ScreenerEngine::checkTrend(
    std::string_view      symbol,
    const ConditionBlock& cond,
    core::Timestamp       cutoff) const
{
    const int lookback = 5;
    auto histRes = repo_->historyBefore(symbol, cutoff, lookback);
    if (!histRes.ok() || histRes.value().size() < 2) return false;

    const auto& rows = histRes.value();  // rows[0] = most recent, rows[n-1] = oldest

    std::string targetStr(cond.value.asString());
    TrendDirection target;
    if      (targetStr == "growing")   target = TrendDirection::growing;
    else if (targetStr == "stable")    target = TrendDirection::stable;
    else if (targetStr == "declining") target = TrendDirection::declining;
    else return false;

    // Count periods where value fell (rows[i] < rows[i+1] means newer < older = a dip)
    int dips = 0;
    for (size_t i = 0; i + 1 < rows.size(); ++i) {
        auto curr = FieldRegistry::resolve(cond.field, rows[i]);
        auto prev = FieldRegistry::resolve(cond.field, rows[i + 1]);
        if (!curr.has_value() || !prev.has_value()) continue;
        if (curr->asDouble() < prev->asDouble()) ++dips;
    }

    switch (target) {
    case TrendDirection::growing:   return dips == 0;    // strictly non-decreasing
    case TrendDirection::stable:    return dips <= 1;    // at most one dip in 5 periods
    case TrendDirection::declining: return dips >= 3;    // 3+ consecutive drops
    }
    return false;
}
```

**Typical use cases:**
- `field="epsGrowth5Y", value="stable"` → EPS CAGR has not dropped more than once in 5 periods
- `field="roe", value="growing"` → ROE has been non-decreasing every period
- `field="fcf", value="declining"` → FCF has fallen 3+ times in 5 periods — potential distress signal

---

## 10. `AppDb` — app.db SQLite Operations (Qt)

### `AppDb.h`

```cpp
#pragma once
#include <filesystem>
#include <optional>
#include <vector>
#include <QString>
#include <QSqlDatabase>
#include "Core/Result.h"
#include "Screener/ConditionBlock.h"
#include "Screener/ScreenerResult.h"

namespace bte::screener {

// Wrapper around app.db (SQLite via Qt).
// Handles screenerTemplates, screenerResults, valuationLists, nlAuditLog.
// All operations are synchronous — call from the UI thread or a dedicated worker.

struct SavedTemplate {
    int                          id;
    QString                      name;
    QString                      mode;         // "builtin" | "python" | "nl"
    ScreenerLogic                logic;
    std::vector<ConditionBlock>  conditions;   // empty for python/nl
    QString                      scriptCode;   // empty for builtin
};

struct SavedResult {
    int                          id;
    QString                      name;
    std::optional<int>           templateId;
    QString                      asOfStart;   // "YYYY-MM-DD"
    QString                      asOfEnd;     // "YYYY-MM-DD"
    QString                      universe;
    std::vector<std::string>     symbols;
    int                          rowCount;
};

class AppDb {
public:
    // Opens (or creates) app.db at the given path.
    // Runs any pending schema migrations automatically.
    static core::Result<std::unique_ptr<AppDb>>
        open(const std::filesystem::path& appDbPath);

    ~AppDb();

    // ── Templates ──────────────────────────────────────────────────────────
    core::Result<int>                    saveTemplate(const SavedTemplate& t);
    core::Result<std::vector<SavedTemplate>> loadAllTemplates() const;
    core::Result<SavedTemplate>          loadTemplate(int id) const;

    // ── Results ────────────────────────────────────────────────────────────
    core::Result<int>                    saveResult(const SavedResult& r);
    core::Result<std::vector<SavedResult>> loadAllResults() const;

    // ── Audit log ──────────────────────────────────────────────────────────
    core::Result<int>  insertAuditEntry(
        const QString& prompt,
        const QString& modelId,
        const QString& sourceHash,
        const QString& generatedCode);

    core::Result<void> markAuditAccepted(int auditId, int templateId);

private:
    explicit AppDb(QSqlDatabase db);

    QSqlDatabase db_;

    core::Result<void> runMigrations();
};

} // namespace bte::screener
```

### `AppDb.cpp` — Key SQL Patterns

#### Opening and migrating

```cpp
core::Result<std::unique_ptr<AppDb>>
AppDb::open(const std::filesystem::path& path)
{
    auto db = QSqlDatabase::addDatabase("QSQLITE", "screener_app");
    db.setDatabaseName(QString::fromStdString(path.string()));

    if (!db.open())
        return core::Error{core::ErrorCode::permissionDenied,
            "Cannot open app.db: " + db.lastError().text().toStdString()};

    auto appDb = std::unique_ptr<AppDb>(new AppDb(db));
    auto migRes = appDb->runMigrations();
    if (!migRes.ok()) return migRes.error();
    return appDb;
}

core::Result<void> AppDb::runMigrations()
{
    QSqlQuery q(db_);

    // Create schemaVersion if absent
    q.exec(R"(
        CREATE TABLE IF NOT EXISTS schemaVersion (
            version    INTEGER NOT NULL,
            appliedAt  DATETIME DEFAULT CURRENT_TIMESTAMP,
            description TEXT
        )
    )");

    // Check current version
    q.exec("SELECT MAX(version) FROM schemaVersion");
    int currentVersion = q.next() ? q.value(0).toInt() : 0;

    if (currentVersion < 1) {
        // Migration 1 — initial schema
        q.exec(R"(
            CREATE TABLE screenerTemplates (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                name       TEXT NOT NULL,
                mode       TEXT NOT NULL CHECK (mode IN ('builtin','python','nl')),
                logic      TEXT CHECK (logic IN ('AND','OR')),
                conditions TEXT,
                scriptCode TEXT,
                nlAuditId  INTEGER REFERENCES nlAuditLog(id),
                createdAt  DATETIME DEFAULT CURRENT_TIMESTAMP,
                updatedAt  DATETIME DEFAULT CURRENT_TIMESTAMP
            )
        )");
        q.exec(R"(
            CREATE TABLE screenerResults (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                name       TEXT NOT NULL,
                templateId INTEGER REFERENCES screenerTemplates(id),
                asOfStart  DATE NOT NULL,
                asOfEnd    DATE NOT NULL,
                universe   TEXT NOT NULL,
                logic      TEXT,
                symbols    TEXT NOT NULL,
                rowCount   INTEGER NOT NULL,
                runAt      DATETIME DEFAULT CURRENT_TIMESTAMP
            )
        )");
        q.exec(R"(
            CREATE TABLE nlAuditLog (
                id            INTEGER PRIMARY KEY AUTOINCREMENT,
                prompt        TEXT NOT NULL,
                modelId       TEXT NOT NULL,
                modelVersion  TEXT,
                sourceHash    TEXT NOT NULL,
                generatedCode TEXT NOT NULL,
                accepted      INTEGER NOT NULL DEFAULT 0,
                acceptedAt    DATETIME,
                templateId    INTEGER REFERENCES screenerTemplates(id),
                resultId      INTEGER REFERENCES screenerResults(id)
            )
        )");
        q.exec(R"(
            INSERT INTO schemaVersion (version, description)
            VALUES (1, 'Initial screener schema')
        )");
    }
    return {};
}
```

#### Saving a template

```cpp
core::Result<int> AppDb::saveTemplate(const SavedTemplate& t)
{
    QSqlQuery q(db_);
    q.prepare(R"(
        INSERT INTO screenerTemplates (name, mode, logic, conditions, scriptCode)
        VALUES (:name, :mode, :logic, :conditions, :scriptCode)
    )");
    q.bindValue(":name",       t.name);
    q.bindValue(":mode",       t.mode);
    q.bindValue(":logic",      t.mode == "builtin"
                                   ? (t.logic == ScreenerLogic::andAll ? "AND" : "OR")
                                   : QVariant(QMetaType::fromType<QString>()));  // NULL
    q.bindValue(":conditions", t.mode == "builtin"
                                   ? serializeConditions(t.conditions)
                                   : QVariant(QMetaType::fromType<QString>()));
    q.bindValue(":scriptCode", t.mode != "builtin"
                                   ? t.scriptCode
                                   : QVariant(QMetaType::fromType<QString>()));

    if (!q.exec())
        return core::Error{core::ErrorCode::internal,
            q.lastError().text().toStdString()};

    return q.lastInsertId().toInt();
}
```

#### Saving a screener result

```cpp
core::Result<int> AppDb::saveResult(const SavedResult& r)
{
    // Serialize symbols list to JSON array string: ["META","NVDA",...]
    QString symbolsJson = "[";
    for (size_t i = 0; i < r.symbols.size(); ++i) {
        if (i > 0) symbolsJson += ",";
        symbolsJson += "\"" + QString::fromStdString(r.symbols[i]) + "\"";
    }
    symbolsJson += "]";

    QSqlQuery q(db_);
    q.prepare(R"(
        INSERT INTO screenerResults
            (name, templateId, asOfStart, asOfEnd, universe, symbols, rowCount)
        VALUES
            (:name, :tid, :start, :end, :uni, :syms, :cnt)
    )");
    q.bindValue(":name",  r.name);
    q.bindValue(":tid",   r.templateId.has_value()
                              ? QVariant(r.templateId.value())
                              : QVariant(QMetaType::fromType<int>()));  // NULL
    q.bindValue(":start", r.asOfStart);
    q.bindValue(":end",   r.asOfEnd);
    q.bindValue(":uni",   r.universe);
    q.bindValue(":syms",  symbolsJson);
    q.bindValue(":cnt",   r.rowCount);

    if (!q.exec())
        return core::Error{core::ErrorCode::internal,
            q.lastError().text().toStdString()};

    return q.lastInsertId().toInt();
}
```

---

## 11. Field Registry

Maps condition `field` keys to `FundamentalsRow` members.  
This is the contract between the UI condition builder and the engine evaluator.

### `FieldRegistry.h`

```cpp
#pragma once
#include <optional>
#include <string>
#include <variant>
#include "Screener/FundamentalsRow.h"

namespace bte::screener {

// A resolved field value: either a numeric double or a string enum.
struct FieldValue {
    std::variant<double, std::string> data;
    double      asDouble() const { return std::get<double>(data); }
    std::string_view asString() const { return std::get<std::string>(data); }
};

class FieldRegistry {
public:
    // Resolves a field key to its value from the given row.
    // Returns nullopt if the field is unknown OR the value is NaN/missing.
    static std::optional<FieldValue> resolve(
        std::string_view       fieldKey,
        const FundamentalsRow& row);
};

} // namespace bte::screener
```

### `FieldRegistry.cpp` — Mapping Table

```cpp
std::optional<FieldValue>
FieldRegistry::resolve(std::string_view key, const FundamentalsRow& row)
{
    using F = FundamentalsRow;

    // ── Fundamental doubles ──────────────────────────────────────────────────
    static const std::unordered_map<std::string_view, double F::*> numericMap = {
        {"roe",           &F::roe},
        {"grossMargin",   &F::grossMargin},
        {"debtEquity",    &F::debtEquity},
        {"fcf",           &F::fcf},
        {"epsGrowth5Y",   &F::epsGrowth5Y},
        {"pe",            &F::pe},
        {"pb",            &F::pb},
        {"ps",            &F::ps},
        {"peg",           &F::peg},
        {"dividendYield", &F::dividendYield},
        {"marketCap",     &F::marketCap},
        {"peAvg5Y",       &F::peAvg5Y},
        {"peAvg10Y",      &F::peAvg10Y},
        {"yieldAvg5Y",    &F::yieldAvg5Y},
        {"yieldAvg10Y",   &F::yieldAvg10Y},
        {"sectorAvgPe",    &F::sectorAvgPe},
        {"sectorAvgPb",    &F::sectorAvgPb},
        {"sectorMedianPs", &F::sectorMedianPs},
    };

    if (auto it = numericMap.find(key); it != numericMap.end()) {
        double v = row.*(it->second);
        if (F::isNaN(v)) return std::nullopt;   // missing → condition fails
        return FieldValue{v};
    }

    // ── String fields ────────────────────────────────────────────────────────
    if (key == "sector") {
        if (row.sector.empty()) return std::nullopt;
        return FieldValue{row.sector};
    }

    // ── Technical derived fields (optional<>) ────────────────────────────────
    if (key == "volumeRatio") {
        if (!row.volumeRatio) return std::nullopt;
        return FieldValue{*row.volumeRatio};
    }
    if (key == "priceVsMa200") {
        if (!row.priceVsMa200) return std::nullopt;
        return FieldValue{*row.priceVsMa200 == PriceVsMa::above
                          ? std::string("ABOVE") : std::string("BELOW")};
    }
    if (key == "priceVsMa20") {
        if (!row.priceVsMa20) return std::nullopt;
        return FieldValue{*row.priceVsMa20 == PriceVsMa::above
                          ? std::string("ABOVE") : std::string("BELOW")};
    }
    if (key == "maAlignment") {
        if (!row.maAlignment) return std::nullopt;
        static const std::array<std::string, 5> names =
            {"BULLISH","PARTIAL_BULL","NEUTRAL","PARTIAL_BEAR","BEARISH"};
        return FieldValue{names[static_cast<int>(*row.maAlignment)]};
    }

    return std::nullopt;  // unknown key → condition fails
}
```

---

## 12. Calling the Engine from Qt UI

### Typical flow in the Qt Widget layer

```cpp
// In ScreenerWidget.cpp — called when user clicks "Run Screen"
void ScreenerWidget::onRunScreenClicked()
{
    // 1. Build request from UI state
    bte::screener::ScreenerRequest req;
    req.universe  = universeCombo_->currentData().toString().toStdString();
    req.asOfEnd   = core::parseDate(asOfEndEdit_->date());
    req.asOfStart = core::parseDate(asOfStartEdit_->date());
    req.logic     = logicToggle_->isAndMode()
                        ? ScreenerLogic::andAll
                        : ScreenerLogic::orAny;
    req.conditions = conditionShelf_->collectConditions();  // ConditionBlock[]

    // 2. Run on worker thread — NEVER block the UI thread
    auto* worker = new ScreenerWorker(engine_, req, this);
    connect(worker, &ScreenerWorker::finished,
            this,   &ScreenerWidget::onScreenerFinished);
    QThreadPool::globalInstance()->start(worker);

    statusLabel_->setText("Running…");
}

void ScreenerWidget::onScreenerFinished(
    core::Result<bte::screener::ScreenerResult> result)
{
    if (!result.ok()) {
        statusLabel_->setText("Error: " + QString::fromStdString(result.error().message));
        return;
    }

    const auto& r = result.value();
    statusLabel_->setText(QString("%1 matches").arg(r.matches.size()));
    resultsModel_->populate(r.matches);   // refreshes QTableView
    resultsTable_->setVisible(true);
}
```

### Saving the result to app.db

```cpp
void ScreenerWidget::onSaveResultClicked()
{
    QString name = QInputDialog::getText(this, "Save List", "List name:");
    if (name.isEmpty()) return;

    bte::screener::SavedResult r;
    r.name       = name;
    r.templateId = currentTemplateId_;   // std::nullopt if unsaved template
    r.asOfStart  = QString::fromStdString(core::toIso8601(currentRequest_.asOfStart));
    r.asOfEnd    = QString::fromStdString(core::toIso8601(currentRequest_.asOfEnd));
    r.universe   = currentRequest_.universe;
    r.rowCount   = static_cast<int>(currentResult_.matches.size());
    for (const auto& m : currentResult_.matches)
        r.symbols.push_back(m.symbol);

    auto res = appDb_->saveResult(r);
    if (!res.ok()) {
        QMessageBox::warning(this, "Save failed", QString::fromStdString(res.error().message));
        return;
    }
    int newId = res.value();
    emit resultSaved(newId, name);   // Block B Source A dropdown refreshes
}
```

---

## 13. Error Handling Reference

| Situation | `ErrorCode` | Behaviour |
|---|---|---|
| `fundamentals` table missing in DuckDB | `schemaMismatch` | `open()` fails; UI shows "Run Python pipeline first" |
| Symbol has no fundamentals ≤ `asOfEnd` | `notFound` | Symbol excluded; added to `ScreenerResult::warnings` |
| Condition field is `NaN` in row | — | `FieldRegistry::resolve` returns `nullopt`; condition = `false` |
| Unknown `field` key in condition | — | `FieldRegistry::resolve` returns `nullopt`; condition = `false` |
| `consecutivePositive` — fewer rows than N | — | `checkConsecutivePositive` returns `false` |
| `app.db` cannot be opened | `permissionDenied` | `AppDb::open` returns error; app cannot save templates |
| `QSqlQuery::exec` fails | `internal` | Propagated as `core::Error`; logged to telemetry |

---

## 14. Cross-References

| This spec section | References |
|---|---|
| §8 DuckDB queries | `Spec_C §3.2` fundamentals schema, `Spec_C §8.2` flow diagram |
| §10 app.db operations | `Spec_C §4.2` screenerTemplates, `Spec_C §4.3` screenerResults |
| §12 Qt worker thread | `02_Frontend_Qt.md §3` threading rules |
| Python / NL execution | `Spec_D §2` (Python screen() API — different code path, same request struct) |
| Result consumed by Block B | `Spec_B §1`, `Spec_C §4.3` screenerResults |
| No-lookahead rule | `11_Stock_Screener_KLine_Product.md §2.3` |
