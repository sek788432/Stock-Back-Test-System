# Spec B — Valuation Engine

**Part of:** Stock Screener sub-specs  
**UI reference:** [`Screener_UI_Overview.md`](./Screener_UI_Overview.md) §B1–B3  
**DB schema:** [`Spec_C_Database.md`](./Spec_C_Database.md)  
**Parent product spec:** [`11_Stock_Screener_KLine_Product.md`](../11_Stock_Screener_KLine_Product.md)  
**Coding conventions:** [`03_Backend_Core.md`](../03_Backend_Core.md) §1  
**Consumes output of:** `Spec_A_Screener_Engine.md` (screenerResults → symbol lists)

> **Scope note:** Block B (Valuation Matrix) is an **extended feature** not explicitly defined in Spec 11. Spec 11 §2 defines screening (Block A) only. Block B was added during sub-spec design as a companion analysis tool that consumes screener output. If Spec 11 is ever revisited, Block B should be promoted to a first-class section there.

---

## 1. Purpose

This spec defines the **computation layer** of Block B (Valuation):

- C++ header definitions for all data structures and interfaces
- Exact formulas for each of the 8 valuation models
- SQL patterns for querying `fundamentals` and `hourlyBars`
- Composite Score calculation
- Technical Signal derivation from price bars
- How the Qt UI calls the engine and reads results

---

## 2. Namespace & File Layout

```
Src/Backend/Valuation/
  ValuationModels.h          — enums + per-model result structs
  ValuationRequest.h         — engine input (symbols + thresholds)
  ValuationResult.h          — engine output (one row per symbol)
  IValuationRepository.h     — pure virtual DB access interface
  IValuationEngine.h         — pure virtual engine interface
  DuckDbValuationRepository.h / .cpp  — MarketData.duckdb implementation
  ValuationEngine.h / .cpp            — engine implementation (8 models + score)
```

All types live in `namespace bte::valuation`.  
Conventions: same as `Spec_A §2` — `UpperCamelCase` types, `lowerCamelCase` members,
private trailing `_`, `#pragma once`, no raw `new`/`delete`, `core::Result<T>`.

---

## 3. Enums & Model Result Types

### `ValuationModels.h`

```cpp
#pragma once
#include <optional>
#include <string>

namespace bte::valuation {

// ── P/E and P/B band classifications ────────────────────────────────────────

enum class ValuationBand {
    lowTrack,    // significantly below sector average  (< 70% of avg)
    midLow,      // below average but not extreme       (70–100% of avg)
    normal,      // near sector average                 (100–130% of avg)
    high         // above sector average                (> 130% of avg)
};

// ── P/S industry level ───────────────────────────────────────────────────────

enum class PsLevel {
    undervalued,  // ps < sector median * 0.8
    normal,       // within ±20% of sector median
    high          // ps > sector median * 1.2
};

// ── Technical signal ────────────────────────────────────────────────────────

enum class TechnicalSignal {
    bullish,   // 🟢 uptrend confirmed
    neutral,   // 🟡 mixed / sideways
    bearish    // 🔴 downtrend confirmed
};

// ── Per-model result ─────────────────────────────────────────────────────────
// Each model produces a value to display and a boolean telling the UI
// whether to apply the green highlight.

struct PegModelResult {
    double peg        = 0.0;
    bool   highlight  = false;  // true if peg < threshold
};

struct DcfModelResult {
    double intrinsicValue  = 0.0;  // USD
    double marginOfSafety  = 0.0;  // percentage, e.g. 22.0 = 22%
    bool   highlight       = false; // true if mos >= threshold
};

struct PeBandResult {
    ValuationBand band      = ValuationBand::normal;
    double        pe        = 0.0;   // current P/E (for display)
    double        sectorAvg = 0.0;   // sector median P/E (for display)
    bool          highlight = false;
};

struct PbBandResult {
    ValuationBand band      = ValuationBand::normal;
    double        pb        = 0.0;
    double        sectorAvg = 0.0;
    bool          highlight = false;
};

struct DdmYieldResult {
    double dividendYield = 0.0;   // decimal (0.058 = 5.8%)
    bool   highlight     = false; // true if yield >= threshold
};

struct PsRatioResult {
    double  ps        = 0.0;
    PsLevel level     = PsLevel::normal;
    bool    highlight = false;
};

struct PeVsAvgResult {
    double pe       = 0.0;
    double avg5Y    = 0.0;
    double avg10Y   = 0.0;
    bool   highlight = false;  // true if pe < avg5Y (below own history)
};

struct YieldVsAvgResult {
    double yield    = 0.0;
    double avg5Y    = 0.0;
    double avg10Y   = 0.0;
    bool   highlight = false;  // true if yield > avg5Y (above own history = cheaper)
};

struct TechnicalSignalResult {
    TechnicalSignal signal  = TechnicalSignal::neutral;
    std::string     label;  // e.g. "MA20_BREAKOUT (Volume Break)"
    double          ma5     = 0.0;
    double          ma20    = 0.0;
    double          ma60    = 0.0;
    double          ma200   = 0.0;
    double          volumeRatio = 0.0;
};

} // namespace bte::valuation
```

---

## 4. `ValuationRequest` & `ValuationResult`

### `ValuationRequest.h`

```cpp
#pragma once
#include <optional>
#include <string>
#include <vector>
#include "Core/Time.h"
#include "Valuation/ValuationModels.h"

namespace bte::valuation {

// User-configurable highlight thresholds (Block B control panel).
// These are display-only: they control green highlights, not data filtering.
struct ValuationThresholds {
    double        pegTarget     = 1.0;            // PEG < this → highlight
    double        dcfMosPct     = 20.0;           // MoS % >= this → highlight
    ValuationBand peBandTarget  = ValuationBand::midLow;  // exact match → highlight
    ValuationBand pbBandTarget  = ValuationBand::lowTrack;
    double        ddmYieldPct   = 4.5;            // yield % >= this → highlight (e.g. 4.5 = 4.5%)
    PsLevel       psLevelTarget = PsLevel::undervalued;
    // Models 7 & 8 (peVsAvg, yieldVsAvg) have no user threshold — auto-comparison
};

// DCF model assumptions — configurable, defaults shown.
struct DcfAssumptions {
    double wacc          = 0.10;  // Weighted Average Cost of Capital (10%)
    double terminalGrowth= 0.03;  // long-run FCF growth rate (3%)
    double nearTermGrowth= 0.0;   // uses epsGrowth5Y from fundamentals if 0.0
    int    forecastYears = 10;
};

struct ValuationRequest {
    std::vector<std::string> symbols;  // symbols to value
    core::Timestamp          asOfEnd;  // no-lookahead cutoff
    ValuationThresholds      thresholds;
    DcfAssumptions           dcfAssumptions;
};

} // namespace bte::valuation
```

### `ValuationResult.h`

```cpp
#pragma once
#include <string>
#include <vector>
#include <optional>
#include "Valuation/ValuationModels.h"

namespace bte::valuation {

struct ValuationRow {
    std::string  symbol;
    std::string  name;          // from stocks table
    int          score = 0;     // Composite Score 0–120 pts (see §11)

    // ── 8 model results ──────────────────────────────────────────────────────
    std::optional<PegModelResult>        peg;
    std::optional<DcfModelResult>        dcf;
    std::optional<PeBandResult>          peBand;
    std::optional<PbBandResult>          pbBand;
    std::optional<DdmYieldResult>        ddmYield;
    std::optional<PsRatioResult>         psRatio;
    std::optional<PeVsAvgResult>         peVsAvg;
    std::optional<YieldVsAvgResult>      yieldVsAvg;
    std::optional<TechnicalSignalResult> technical;
    // nullopt means the required data was unavailable for that model
};

struct ValuationResult {
    std::vector<ValuationRow>  rows;
    std::vector<std::string>   warnings;   // missing data per symbol
};

} // namespace bte::valuation
```

---

## 5. `IValuationRepository` — DB Access Interface

### `IValuationRepository.h`

```cpp
#pragma once
#include <string>
#include <vector>
#include "Core/Result.h"
#include "Core/Time.h"

namespace bte::valuation {

// Data the engine needs per symbol from MarketData.duckdb.
// Fetched once per symbol per request and cached for the request lifetime.
struct SymbolData {
    // From fundamentals (latest row <= asOfEnd)
    double pe           = 0.0;
    double pb           = 0.0;
    double ps           = 0.0;
    double peg          = 0.0;
    double fcf          = 0.0;
    double dividendYield= 0.0;
    double marketCap    = 0.0;
    double epsGrowth5Y  = 0.0;
    double sectorAvgPe  = 0.0;
    double sectorAvgPb  = 0.0;
    double sectorMedianPs    = 0.0;   // from fundamentals (pre-computed by Python pipeline — see API_Data_Requirements.md §2.2E)
    double sharesOutstanding = 0.0;   // diluted shares — required for per-share DCF (Spec_C §3.2)
    double peAvg5Y      = 0.0;
    double peAvg10Y     = 0.0;
    double yieldAvg5Y   = 0.0;
    double yieldAvg10Y  = 0.0;
    std::string sector;
    std::string name;

    // From hourlyBars (last bar <= asOfEnd)
    double lastPrice    = 0.0;
    double changePercent= 0.0;

    // From hourlyBars (last 20–200 bars <= asOfEnd, for technical)
    double ma5    = 0.0;
    double ma20   = 0.0;
    double ma60   = 0.0;
    double ma200  = 0.0;
    double volumeRatio = 0.0;   // volume / 20d avg volume
};

class IValuationRepository {
public:
    virtual ~IValuationRepository() = default;

    // Fetches all data needed for one symbol in one call.
    virtual core::Result<SymbolData> fetchSymbolData(
        std::string_view symbol,
        core::Timestamp  asOfEnd) const = 0;

    // Returns display name from stocks table.
    virtual core::Result<std::string> fetchName(
        std::string_view symbol) const = 0;
};

} // namespace bte::valuation
```

---

## 6. DB Queries — `DuckDbValuationRepository`

### `DuckDbValuationRepository.cpp` — Key SQL Patterns

#### `fetchSymbolData` — combined fundamentals + bars query

```cpp
core::Result<SymbolData>
DuckDbValuationRepository::fetchSymbolData(
    std::string_view sym, core::Timestamp cutoff) const
{
    SymbolData d;

    // ── Step 1: fundamentals (no-lookahead) ─────────────────────────────────
    auto fRes = conn_.Query(R"(
        SELECT
            pe, pb, ps, peg, fcf, dividendYield, marketCap, epsGrowth5Y,
            sectorAvgPe, sectorAvgPb, sectorMedianPs,
            peAvg5Y, peAvg10Y, yieldAvg5Y, yieldAvg10Y,
            sharesOutstanding, sector
        FROM fundamentals
        WHERE symbol   = ?
          AND asOfDate <= ?
        ORDER BY asOfDate DESC
        LIMIT 1
    )", sym, core::toIso8601(cutoff));

    if (fRes->RowCount() == 0)
        return core::Error{core::ErrorCode::notFound,
            "no fundamentals for " + std::string(sym)};

    auto fc = fRes->Fetch();
    d.pe            = fc->GetValue<double>(0,  0);
    d.pb            = fc->GetValue<double>(1,  0);
    d.ps            = fc->GetValue<double>(2,  0);
    d.peg           = fc->GetValue<double>(3,  0);
    d.fcf           = fc->GetValue<double>(4,  0);
    d.dividendYield = fc->GetValue<double>(5,  0);
    d.marketCap     = fc->GetValue<double>(6,  0);
    d.epsGrowth5Y   = fc->GetValue<double>(7,  0);
    d.sectorAvgPe        = fc->GetValue<double>(8,  0);
    d.sectorAvgPb        = fc->GetValue<double>(9,  0);
    d.sectorMedianPs     = fc->GetValue<double>(10, 0);
    d.peAvg5Y            = fc->GetValue<double>(11, 0);
    d.peAvg10Y           = fc->GetValue<double>(12, 0);
    d.yieldAvg5Y         = fc->GetValue<double>(13, 0);
    d.yieldAvg10Y        = fc->GetValue<double>(14, 0);
    d.sharesOutstanding  = fc->GetValue<double>(15, 0);
    d.sector             = fc->GetValue<std::string>(16, 0);

    // sectorMedianPs is pre-computed by the Python pipeline and stored in fundamentals (Spec_C §3.2).
    // No on-the-fly cross-sectional query needed here.

    // ── Step 2: last price + change % (from hourlyBars) ──────────────────────
    auto priceRes = conn_.Query(R"(
        WITH ranked AS (
            SELECT close, ts,
                   LAG(close) OVER (PARTITION BY symbol ORDER BY ts) AS prevClose
            FROM hourlyBars
            WHERE symbol     = ?
              AND schemaName = 'ohlcv-1d'
              AND ts        <= ?
        )
        SELECT close, (close - prevClose) / prevClose * 100.0
        FROM ranked
        WHERE prevClose IS NOT NULL
        ORDER BY ts DESC
        LIMIT 1
    )", sym, core::toIso8601(cutoff));

    if (priceRes->RowCount() > 0) {
        auto pc = priceRes->Fetch();
        d.lastPrice     = pc->GetValue<double>(0, 0);
        d.changePercent = pc->GetValue<double>(1, 0);
    }

    // ── Step 3: moving averages + volume ratio (last 200 daily bars) ─────────
    auto maRes = conn_.Query(R"(
        SELECT
            AVG(close) FILTER (WHERE rn <= 5)   AS ma5,
            AVG(close) FILTER (WHERE rn <= 20)  AS ma20,
            AVG(close) FILTER (WHERE rn <= 60)  AS ma60,
            AVG(close) FILTER (WHERE rn <= 200) AS ma200,
            MAX(volume) FILTER (WHERE rn = 1) /
                NULLIF(AVG(volume) FILTER (WHERE rn <= 20), 0) AS volumeRatio
        FROM (
            SELECT close, volume,
                   ROW_NUMBER() OVER (ORDER BY ts DESC) AS rn
            FROM hourlyBars
            WHERE symbol     = ?
              AND schemaName = 'ohlcv-1d'
              AND ts        <= ?
            LIMIT 200
        )
    )", sym, core::toIso8601(cutoff));

    if (maRes->RowCount() > 0) {
        auto mc = maRes->Fetch();
        d.ma5         = mc->GetValue<double>(0, 0);
        d.ma20        = mc->GetValue<double>(1, 0);
        d.ma60        = mc->GetValue<double>(2, 0);
        d.ma200       = mc->GetValue<double>(3, 0);
        d.volumeRatio = mc->GetValue<double>(4, 0);
    }

    return d;
}
```

---

## 7. The Eight Valuation Models

### `IValuationEngine.h`

```cpp
#pragma once
#include <memory>
#include "Core/Result.h"
#include "Valuation/IValuationRepository.h"
#include "Valuation/ValuationRequest.h"
#include "Valuation/ValuationResult.h"

namespace bte::valuation {

class IValuationEngine {
public:
    virtual ~IValuationEngine() = default;

    static core::Result<std::unique_ptr<IValuationEngine>> create(
        std::shared_ptr<IValuationRepository> repo);

    // Compute all 8 models + composite score for every symbol in the request.
    // Blocking — call from a worker thread.
    virtual core::Result<ValuationResult> run(
        const ValuationRequest& req) const = 0;
};

} // namespace bte::valuation
```

### `ValuationEngine.cpp` — Model implementations

---

#### Model 1 — PEG Ratio

**Formula:**
```
peg = pe / (epsGrowth5Y * 100)
highlight = (peg < thresholds.pegTarget)
```

`epsGrowth5Y` is a decimal (e.g. `0.18` = 18% growth) — multiply by 100 before dividing.

```cpp
std::optional<PegModelResult>
ValuationEngine::computePeg(const SymbolData& d,
                            const ValuationThresholds& th)
{
    if (d.pe <= 0 || d.epsGrowth5Y <= 0) return std::nullopt;

    PegModelResult r;
    r.peg       = d.pe / (d.epsGrowth5Y * 100.0);
    r.highlight = r.peg < th.pegTarget;
    return r;
}
```

**Example — META:** `pe=22.4, epsGrowth5Y=0.27 (27%)`
```
peg = 22.4 / 27.0 = 0.83
threshold = 1.0  →  0.83 < 1.0  →  highlight = true ✓
```

---

#### Model 2 — DCF + Margin of Safety

**Formula (two-stage DCF):**
```
g1 = epsGrowth5Y  (near-term FCF growth, years 1–forecastYears)
g2 = terminalGrowth
WACC = wacc

PV_stage1 = Σ(t=1..N)  FCF * (1+g1)^t / (1+WACC)^t
PV_terminal = [ FCF * (1+g1)^N * (1+g2) / (WACC - g2) ] / (1+WACC)^N

intrinsicValue = PV_stage1 + PV_terminal
marginOfSafety = (intrinsicValue - lastPrice) / intrinsicValue * 100
highlight = (marginOfSafety >= thresholds.dcfMosPct)
```

```cpp
std::optional<DcfModelResult>
ValuationEngine::computeDcf(const SymbolData& d,
                             const ValuationThresholds& th,
                             const DcfAssumptions& dcf)
{
    if (d.fcf <= 0 || d.lastPrice <= 0 || d.sharesOutstanding <= 0) return std::nullopt;

    // Convert total FCF to per-share FCF for comparison with lastPrice (also per-share)
    const double fcfPerShare = d.fcf / d.sharesOutstanding;

    double g1   = dcf.nearTermGrowth > 0.0 ? dcf.nearTermGrowth : d.epsGrowth5Y;
    double g2   = dcf.terminalGrowth;
    double wacc = dcf.wacc;
    int    n    = dcf.forecastYears;

    if (wacc <= g2) return std::nullopt;  // Gordon model undefined if WACC <= g

    // Stage 1: PV of growing per-share FCF for n years
    double pv1 = 0.0;
    for (int t = 1; t <= n; ++t)
        pv1 += fcfPerShare * std::pow(1.0 + g1, t) / std::pow(1.0 + wacc, t);

    // Stage 2: terminal value (Gordon Growth) discounted back n years
    double terminalFcf = fcfPerShare * std::pow(1.0 + g1, n) * (1.0 + g2);
    double pv2 = (terminalFcf / (wacc - g2)) / std::pow(1.0 + wacc, n);

    double intrinsic = pv1 + pv2;
    double mos = (intrinsic - d.lastPrice) / intrinsic * 100.0;

    DcfModelResult r;
    r.intrinsicValue = intrinsic;
    r.marginOfSafety = mos;
    r.highlight      = mos >= th.dcfMosPct;
    return r;
}
```

**Example — META:** `fcf=5.2e10, sharesOutstanding=2.57e9, lastPrice=597.42, epsGrowth5Y=0.27, WACC=0.10, g2=0.03, n=10`
```
fcfPerShare = 52,000,000,000 / 2,570,000,000 = $20.23/share

Stage 1 PV ≈ $108/share
Stage 2 PV ≈ $352/share
intrinsicValue ≈ $460/share
MoS = (460 - 597.42) / 460 × 100 = −29.9%   ← price ABOVE intrinsic (overvalued by DCF)
threshold = 20%  →  −29.9 >= 20 → highlight = false
```

---

#### Model 3 — P/E Valuation Band

**Formula:**
```
ratio = pe / sectorAvgPe

lowTrack  : ratio < 0.70
midLow    : 0.70 <= ratio < 1.00
normal    : 1.00 <= ratio < 1.30
high      : ratio >= 1.30

highlight = (band == thresholds.peBandTarget)
```

```cpp
std::optional<PeBandResult>
ValuationEngine::computePeBand(const SymbolData& d,
                               const ValuationThresholds& th)
{
    if (d.pe <= 0 || d.sectorAvgPe <= 0) return std::nullopt;

    double ratio = d.pe / d.sectorAvgPe;

    PeBandResult r;
    r.pe        = d.pe;
    r.sectorAvg = d.sectorAvgPe;

    if      (ratio < 0.70) r.band = ValuationBand::lowTrack;
    else if (ratio < 1.00) r.band = ValuationBand::midLow;
    else if (ratio < 1.30) r.band = ValuationBand::normal;
    else                   r.band = ValuationBand::high;

    r.highlight = (r.band == th.peBandTarget);
    return r;
}
```

**Example — META:** `pe=22.4, sectorAvgPe=28.1`
```
ratio = 22.4 / 28.1 = 0.797  →  band = midLow
threshold = midLow  →  highlight = true ✓
```

**Example — AAPL:** `pe=30.5, sectorAvgPe=28.1`
```
ratio = 30.5 / 28.1 = 1.085  →  band = normal
threshold = midLow  →  highlight = false
```

---

#### Model 4 — P/B Valuation Band

**Same structure as Model 3, using `pb` and `sectorAvgPb`.**

```cpp
std::optional<PbBandResult>
ValuationEngine::computePbBand(const SymbolData& d,
                               const ValuationThresholds& th)
{
    if (d.pb <= 0 || d.sectorAvgPb <= 0) return std::nullopt;

    double ratio = d.pb / d.sectorAvgPb;

    PbBandResult r;
    r.pb        = d.pb;
    r.sectorAvg = d.sectorAvgPb;

    if      (ratio < 0.70) r.band = ValuationBand::lowTrack;
    else if (ratio < 1.00) r.band = ValuationBand::midLow;
    else if (ratio < 1.30) r.band = ValuationBand::normal;
    else                   r.band = ValuationBand::high;

    r.highlight = (r.band == th.pbBandTarget);
    return r;
}
```

**Example — O (Realty Income):** `pb=1.20, sectorAvgPb=1.85`
```
ratio = 1.20 / 1.85 = 0.649  →  band = lowTrack
threshold = lowTrack  →  highlight = true ✓
```

---

#### Model 5 — DDM Yield

**Formula:**
```
yield = dividendYield   (already a decimal from fundamentals, e.g. 0.058 = 5.8%)
displayYield = yield * 100   (for UI: "5.8%")
highlight = (displayYield >= thresholds.ddmYieldPct)
```

```cpp
std::optional<DdmYieldResult>
ValuationEngine::computeDdmYield(const SymbolData& d,
                                  const ValuationThresholds& th)
{
    if (d.dividendYield < 0) return std::nullopt;

    DdmYieldResult r;
    r.dividendYield = d.dividendYield * 100.0;   // convert to % for display
    r.highlight     = r.dividendYield >= th.ddmYieldPct;
    return r;
}
```

**Example — O:** `dividendYield=0.058`
```
displayYield = 5.8%
threshold = 4.5%  →  5.8 >= 4.5  →  highlight = true ✓
```

**Example — META:** `dividendYield=0.004`
```
displayYield = 0.4%
threshold = 4.5%  →  0.4 >= 4.5  →  highlight = false
```

---

#### Model 6 — P/S Ratio vs Industry

**Formula:**
```
ratio = ps / sectorMedianPs

undervalued : ratio < 0.80
normal      : 0.80 <= ratio < 1.20
high        : ratio >= 1.20

highlight = (level == thresholds.psLevelTarget)
```

```cpp
std::optional<PsRatioResult>
ValuationEngine::computePsRatio(const SymbolData& d,
                                 const ValuationThresholds& th)
{
    if (d.ps <= 0 || d.sectorMedianPs <= 0) return std::nullopt;

    double ratio = d.ps / d.sectorMedianPs;

    PsRatioResult r;
    r.ps = d.ps;

    if      (ratio < 0.80) r.level = PsLevel::undervalued;
    else if (ratio < 1.20) r.level = PsLevel::normal;
    else                   r.level = PsLevel::high;

    r.highlight = (r.level == th.psLevelTarget);
    return r;
}
```

**Example — META:** `ps=3.2, sectorMedianPs=5.1`
```
ratio = 3.2 / 5.1 = 0.627  →  level = undervalued
threshold = undervalued  →  highlight = true ✓
```

---

#### Model 7 — P/E vs Historical Average

**Formula:**
```
highlight = (pe < peAvg5Y)   ← stock is cheaper than its own 5Y average
display   = "22.4x / 5Y:25.2x / 10Y:28.1x"
```

The direction is: **lower P/E relative to own history = potentially undervalued**.

```cpp
std::optional<PeVsAvgResult>
ValuationEngine::computePeVsAvg(const SymbolData& d)
{
    if (d.pe <= 0 || d.peAvg5Y <= 0 || d.peAvg10Y <= 0)
        return std::nullopt;

    PeVsAvgResult r;
    r.pe        = d.pe;
    r.avg5Y     = d.peAvg5Y;
    r.avg10Y    = d.peAvg10Y;
    r.highlight = d.pe < d.peAvg5Y;  // below own 5Y average
    return r;
}
```

**Example — META:** `pe=22.4, peAvg5Y=25.2, peAvg10Y=28.1`
```
22.4 < 25.2  →  highlight = true ✓  (trading below 5Y avg — historically cheap)
```

**Example — AAPL:** `pe=30.5, peAvg5Y=27.3, peAvg10Y=22.1`
```
30.5 < 27.3  →  false  →  highlight = false  (currently expensive vs own history)
```

---

#### Model 8 — Dividend Yield vs Historical Average

**Formula:**
```
highlight = (dividendYield > yieldAvg5Y) AND (dividendYield > 0)
display   = "5.8% / 5Y:4.9% / 10Y:4.6%"
```

**Direction is inverted vs Model 7:**  
Higher yield vs history means the **price fell** relative to the dividend.  
A rising yield = stock is getting cheaper. This is a buy signal for income investors.

```cpp
std::optional<YieldVsAvgResult>
ValuationEngine::computeYieldVsAvg(const SymbolData& d)
{
    if (d.dividendYield < 0 || d.yieldAvg5Y <= 0 || d.yieldAvg10Y <= 0)
        return std::nullopt;

    YieldVsAvgResult r;
    r.yield     = d.dividendYield * 100.0;   // % for display
    r.avg5Y     = d.yieldAvg5Y   * 100.0;
    r.avg10Y    = d.yieldAvg10Y  * 100.0;
    r.highlight = d.dividendYield > d.yieldAvg5Y
               && d.dividendYield > 0.0;
    return r;
}
```

**Example — O:** `dividendYield=0.058, yieldAvg5Y=0.049`
```
0.058 > 0.049  →  highlight = true ✓  (yield above 5Y avg → price fell → cheaper)
```

**Example — MSFT:** `dividendYield=0.007, yieldAvg5Y=0.012`
```
0.007 > 0.012  →  false  →  highlight = false  (yield below avg → price is high)
```

---

#### Technical Signal (Decoupled — not a valuation model)

Technical signal is displayed separately from the 8 valuation models and does **not affect Composite Score**. It uses only `hourlyBars` data.

**Rules:**

```
BULLISH:      close > ma20  AND  ma5 > ma20  AND  ma20 > ma60
BEARISH:      close < ma20  AND  ma5 < ma20  AND  ma20 < ma60
NEUTRAL:      all other combinations

Volume override label:
  if volumeRatio >= 2.0 AND signal == BULLISH  →  label includes "(Volume Break)"
  if volumeRatio >= 2.0 AND signal == BEARISH  →  label includes "(Volume Sell-off)"
```

```cpp
std::optional<TechnicalSignalResult>
ValuationEngine::computeTechnical(const SymbolData& d)
{
    if (d.ma5 <= 0 || d.ma20 <= 0 || d.ma60 <= 0) return std::nullopt;

    TechnicalSignalResult r;
    r.ma5         = d.ma5;
    r.ma20        = d.ma20;
    r.ma60        = d.ma60;
    r.ma200       = d.ma200;
    r.volumeRatio = d.volumeRatio;

    bool priceAboveMa20 = d.lastPrice > d.ma20;
    bool ma5AboveMa20   = d.ma5  > d.ma20;
    bool ma20AboveMa60  = d.ma20 > d.ma60;

    if (priceAboveMa20 && ma5AboveMa20 && ma20AboveMa60) {
        r.signal = TechnicalSignal::bullish;
        r.label  = d.volumeRatio >= 2.0
                   ? "MA20_BREAKOUT (Volume Break)"
                   : "TREND_UP (Bullish MA Sequence)";
    } else if (!priceAboveMa20 && !ma5AboveMa20 && !ma20AboveMa60) {
        r.signal = TechnicalSignal::bearish;
        r.label  = d.volumeRatio >= 2.0
                   ? "BEAR_BREAKDOWN (Volume Sell-off)"
                   : "BEAR_ALIGNMENT (Bearish MA Sequence)";
    } else {
        r.signal = TechnicalSignal::neutral;
        r.label  = d.volumeRatio >= 2.0
                   ? "HIGH_VOLATILITY (Volume Divergence)"
                   : "SIDE_TREND (Sideways Compression)";
    }
    return r;
}
```

---

## 8. Composite Score

### Formula

```
Base score (from DCF Margin of Safety):
  MoS >= 30%  →  50 pts
  MoS >= 15%  →  35 pts
  MoS <  15% (or DCF unavailable)  →  10 pts

Per-model bonus — each highlighted model adds +10 pts:
  Model 1  PEG highlight       →  +10
  Model 3  P/E band match      →  +10
  Model 4  P/B band match      →  +10
  Model 5  DDM yield match     →  +10
  Model 6  P/S level match     →  +10
  Model 7  P/E below 5Y avg    →  +10
  Model 8  Yield above 5Y avg  →  +10

Cap: 120 pts
```

Note: Model 2 (DCF) is the base score driver, not a flat +10 bonus.  
Technical Signal is explicitly excluded from the score.

```cpp
int ValuationEngine::computeScore(const ValuationRow& row)
{
    // Base score from DCF margin of safety
    int score = 10;
    if (row.dcf.has_value()) {
        double mos = row.dcf->marginOfSafety;
        if      (mos >= 30.0) score = 50;
        else if (mos >= 15.0) score = 35;
    }

    // +10 per highlighted model (excluding DCF base and Technical)
    auto add = [&](bool highlighted) { if (highlighted) score += 10; };
    if (row.peg)        add(row.peg->highlight);
    if (row.peBand)     add(row.peBand->highlight);
    if (row.pbBand)     add(row.pbBand->highlight);
    if (row.ddmYield)   add(row.ddmYield->highlight);
    if (row.psRatio)    add(row.psRatio->highlight);
    if (row.peVsAvg)    add(row.peVsAvg->highlight);
    if (row.yieldVsAvg) add(row.yieldVsAvg->highlight);

    return std::min(score, 120);
}
```

### Worked Example — META

```
DCF MoS = -29.9%  →  base = 10 pts
PEG = 0.83 < 1.0  →  +10
P/E band = midLow (matches threshold)  →  +10
P/B band = NORMAL (does NOT match lowTrack)  →  +0
DDM yield = 0.4% < 4.5%  →  +0
P/S = undervalued (matches)  →  +10
P/E vs avg: 22.4 < 25.2  →  +10
Yield vs avg: 0.4% < 0.2%  →  +0  (yield not above avg)

Score = 10 + 10 + 10 + 10 + 10 = 50 pts
```

### Worked Example — O (Realty Income)

```
DCF MoS = 12.0%  →  base = 10 pts
PEG = 2.1 > 1.0  →  +0
P/E band = NORMAL (does not match midLow)  →  +0
P/B band = lowTrack (matches)  →  +10
DDM yield = 5.8% >= 4.5%  →  +10
P/S = NORMAL (does not match undervalued)  →  +0
P/E vs avg: 45.0 < 48.2  →  +10
Yield vs avg: 5.8% > 4.9%  →  +10

Score = 10 + 10 + 10 + 10 + 10 = 50 pts
```

---

## 9. Full Engine Run Loop

### `ValuationEngine.cpp`

```cpp
core::Result<ValuationResult>
ValuationEngine::run(const ValuationRequest& req) const
{
    ValuationResult result;

    for (const auto& sym : req.symbols) {
        // 1. Fetch all data for this symbol in one repository call
        auto dataRes = repo_->fetchSymbolData(sym, req.asOfEnd);
        if (!dataRes.ok()) {
            result.warnings.push_back("no data for " + sym + ": "
                                      + dataRes.error().message);
            continue;
        }
        const SymbolData& d = dataRes.value();

        // 2. Compute all 8 models
        ValuationRow row;
        row.symbol     = sym;
        row.name       = d.name;
        row.peg        = computePeg(d, req.thresholds);
        row.dcf        = computeDcf(d, req.thresholds, req.dcfAssumptions);
        row.peBand     = computePeBand(d, req.thresholds);
        row.pbBand     = computePbBand(d, req.thresholds);
        row.ddmYield   = computeDdmYield(d, req.thresholds);
        row.psRatio    = computePsRatio(d, req.thresholds);
        row.peVsAvg    = computePeVsAvg(d);
        row.yieldVsAvg = computeYieldVsAvg(d);
        row.technical  = computeTechnical(d);

        // 3. Composite score
        row.score = computeScore(row);

        result.rows.push_back(std::move(row));
    }

    return result;
}
```

---

## 10. Calling the Engine from Qt UI

```cpp
// In ValuationWidget.cpp — called when thresholds change or new symbols loaded
void ValuationWidget::recompute()
{
    // Build request from UI state
    bte::valuation::ValuationRequest req;
    req.symbols    = activeSymbolList_;       // maintained by Source A/B/C logic
    req.asOfEnd    = core::parseDate(asOfEndDate_);
    req.thresholds = readThresholdsFromControls();  // reads 6 UI spinboxes/dropdowns

    // Run on worker thread
    auto* worker = new ValuationWorker(engine_, req, this);
    connect(worker, &ValuationWorker::finished,
            this,   &ValuationWidget::onValuationFinished);
    QThreadPool::globalInstance()->start(worker);
}

void ValuationWidget::onValuationFinished(
    core::Result<bte::valuation::ValuationResult> result)
{
    if (!result.ok()) { showError(result.error()); return; }
    valuationModel_->populate(result.value().rows);  // refreshes QTableView
}
```

**When the user changes a threshold control (oninput / onchange equivalent in Qt):**

```cpp
void ValuationWidget::onThresholdChanged()
{
    // Thresholds only affect highlight state, not which data is loaded.
    // We can recompute synchronously from cached SymbolData — no new DB query.
    auto thresholds = readThresholdsFromControls();
    valuationModel_->reapplyHighlights(thresholds);  // in-memory, no engine call
}
```

---

## 11. Error Handling Reference

| Situation | Behaviour |
|---|---|
| Symbol has no fundamentals ≤ `asOfEnd` | `ValuationRow` skipped; added to `warnings` |
| `fcf <= 0` or `lastPrice <= 0` | `dcf` = `nullopt` (DCF undefined); base score = 10 pts |
| `epsGrowth5Y <= 0` | `peg` = `nullopt` |
| `sectorAvgPe <= 0` | `peBand` = `nullopt` |
| `sectorMedianPs <= 0` | `psRatio` = `nullopt` |
| `dividendYield == 0` | `ddmYield` shown with highlight=false; `yieldVsAvg` = `nullopt` |
| MA data unavailable (< 5 bars) | `technical` = `nullopt`; cell shown empty in UI |
| `wacc <= terminalGrowth` in DCF | `dcf` = `nullopt`; log config warning |

---

## 12. Cross-References

| This spec section | References |
|---|---|
| §6 DB queries | `Spec_C §3.1` hourlyBars, `Spec_C §3.2` fundamentals |
| §7 Model formulas | `Screener_UI_Overview.md §B3` column definitions |
| §8 Composite Score formula | `Screener_UI_Overview.md §B3` Score Formula section |
| §10 Qt integration | `02_Frontend_Qt.md §3` threading, `Spec_C §4.4` valuationLists |
| Symbol lists consumed | `Spec_A §9` ScreenerResult, `Spec_C §4.3` screenerResults |
| NL/Python data access | `Spec_D §2.2` bars DataFrame columns (same fundamentals join) |
