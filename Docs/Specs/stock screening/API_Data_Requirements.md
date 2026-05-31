# API Data Requirements — Stock Screener

**Part of:** Stock Screener sub-specs  
**Audience:** Python pipeline engineers — what to fetch, compute, and store in `MarketData.duckdb`  
**DB schema:** [`Spec_C_Database.md`](./Spec_C_Database.md)  
**Referenced by:** `Spec_B_Valuation_Engine.md` (model inputs), `Spec_D_NL_Python_Runtime.md` (bars DataFrame)

---

## 1. Overview

The screener reads from two tables in `MarketData.duckdb`. Both are owned and written by the Python pipeline.

```
External APIs
    │
    ├─► Daily OHLCV bars      ─────────────► hourlyBars
    │
    └─► Quarterly financials  ─► compute ──► fundamentals
                                    │
                                    └── compute cross-sectional ──► fundamentals (sectorAvg*)
```

**Two kinds of fields in `fundamentals`:**

| Kind | Examples | Source |
|---|---|---|
| **Raw from API** | `roe`, `fcf`, `pe`, `dividendYield` | Fetched or trivially derived from API |
| **Computed by pipeline** | `epsGrowth5Y`, `peAvg5Y`, `peAvg10Y`, `yieldAvg5Y` | Require 5–10 years of stored quarterly history |
| **Cross-sectional** | `sectorAvgPe`, `sectorAvgPb`, `sectorMedianPs` | Computed across peers on same `asOfDate` |

**Critical rule:** Most free APIs do **not** provide historical P/E or yield averages. The pipeline must reconstruct them by computing PE from (quarterly EPS × closing price on filing date) and then averaging over N years of stored rows.

---

## 2. Complete Field List

### 2.1 `hourlyBars` — Daily Price & Volume

One row per symbol per trading day.

| DB Column | Type | Raw API Field | Notes |
|---|---|---|---|
| `symbol` | TEXT | ticker | e.g. `"AAPL"` |
| `ts` | TIMESTAMPTZ | trade date | stored as UTC market open |
| `open` | DOUBLE | open price | |
| `high` | DOUBLE | daily high | |
| `low` | DOUBLE | daily low | |
| `close` | DOUBLE | closing price | adjusted for splits |
| `volume` | BIGINT | shares traded | |
| `schemaName` | TEXT | — | always `'ohlcv-1d'` for daily bars |

**Derived at query time (NOT stored):** MA5, MA20, MA60, MA200, volumeRatio — computed by the screener engine or Python `screen()` from the bars themselves.

---

### 2.2 `fundamentals` — Per-Symbol Financial Metrics

One row per symbol per fiscal quarter end date (`asOfDate`).

#### A. Valuation Ratios — fetch directly or compute from raw

| DB Column | Formula / Source | Raw API fields needed |
|---|---|---|
| `pe` | `price / TTM_EPS` | quarterly EPS (×4 quarters) + close price on filing date |
| `pb` | `price / (total_equity / shares_outstanding)` | total equity, shares outstanding, close price |
| `ps` | `price / (TTM_revenue / shares_outstanding)` | quarterly revenue (×4) + shares outstanding + close price |
| `peg` | `pe / (epsGrowth5Y × 100)` | computed from `pe` + `epsGrowth5Y` — no extra API call |
| `dividendYield` | `annual_dividend / price` | dividends per share (annualised) + close price |

#### B. Quality Metrics — fetch from financial statements

| DB Column | Formula / Source | Raw API fields needed |
|---|---|---|
| `roe` | `TTM_net_income / avg_equity` | income statement: net income; balance sheet: equity |
| `grossMargin` | `TTM_gross_profit / TTM_revenue` | income statement: gross profit, revenue |
| `debtEquity` | `total_debt / total_equity` | balance sheet: total debt, total equity |
| `fcf` | `operating_cash_flow − capex` | cash flow statement: operating CF, capital expenditure |
| `epsGrowth5Y` | `(EPS_now / EPS_5Y_ago)^(1/5) − 1` | **5 years of annual EPS** — requires stored history |

#### C. DCF Inputs

| DB Column | Formula / Source | Raw API fields needed |
|---|---|---|
| `marketCap` | `shares_outstanding × close_price` | shares outstanding + close price |
| `revenue` | TTM revenue (USD) | income statement: revenue (×4 quarters) |
| `sharesOutstanding` | diluted shares outstanding | balance sheet / income statement footer |

> `sharesOutstanding` is required for the DCF model to convert total-company FCF into per-share intrinsic value.

#### D. Historical Averages — computed by pipeline from stored quarterly rows

These fields **cannot be fetched from any standard API**. They must be computed by looking back at the pipeline's own stored `fundamentals` rows.

| DB Column | Formula | History required |
|---|---|---|
| `peAvg5Y` | `mean(pe)` over last 20 quarterly rows | **5 years = 20 quarters** of `pe` per symbol |
| `peAvg10Y` | `mean(pe)` over last 40 quarterly rows | **10 years = 40 quarters** of `pe` per symbol |
| `yieldAvg5Y` | `mean(dividendYield)` over last 20 rows | 5 years of `dividendYield` per symbol |
| `yieldAvg10Y` | `mean(dividendYield)` over last 40 rows | 10 years of `dividendYield` per symbol |

**Python pipeline pseudocode:**
```python
# After inserting new quarterly row for symbol:
pe_history = db.query(
    "SELECT pe FROM fundamentals WHERE symbol=? AND asOfDate <= ? ORDER BY asOfDate DESC LIMIT 20",
    symbol, as_of_date
)
peAvg5Y = mean(pe_history['pe'].dropna())

pe_history_10y = db.query("... LIMIT 40", symbol, as_of_date)
peAvg10Y = mean(pe_history_10y['pe'].dropna())
```

#### E. Cross-Sectional Context — computed across peer group on same `asOfDate`

These must be computed **after all symbols for the same quarter are ingested**, in a single pass.

| DB Column | Formula | When to compute |
|---|---|---|
| `sectorAvgPe` | `median(pe)` of all symbols in same sector, same `asOfDate` | After all symbols for that quarter are inserted |
| `sectorAvgPb` | `median(pb)` across peers | Same |
| `sectorMedianPs` | `median(ps)` across peers | Same |

**Python pipeline pseudocode:**
```python
# After all symbols for a given asOfDate are inserted:
sector_stats = db.query("""
    SELECT sector,
           MEDIAN(pe) AS sectorAvgPe,
           MEDIAN(pb) AS sectorAvgPb,
           MEDIAN(ps) AS sectorMedianPs
    FROM fundamentals
    WHERE asOfDate = ?
    GROUP BY sector
""", as_of_date)

for row in sector_stats:
    db.execute("""
        UPDATE fundamentals
        SET sectorAvgPe=?, sectorAvgPb=?, sectorMedianPs=?
        WHERE sector=? AND asOfDate=?
    """, row.sectorAvgPe, row.sectorAvgPb, row.sectorMedianPs, row.sector, as_of_date)
```

---

### 2.3 `stocks` — Symbol Master Data

Static reference; update when symbols are added or removed from the universe.

| DB Column | Source |
|---|---|
| `symbol` | API ticker (e.g. `"AAPL"`) |
| `name` | Full company name (e.g. `"Apple Inc."`) |
| `exchange` | `'NASDAQ'` or `'NYSE'` |
| `country` | `'US'` (hardcoded for Phase 1) |

**Phase 2 addition — Index Membership Table** (not in stocks, separate table):
```sql
CREATE TABLE indexConstituents (
    symbol      TEXT NOT NULL,
    indexName   TEXT NOT NULL,   -- 'SP500', 'NASDAQ100', 'NYSE'
    effectiveDate DATE NOT NULL,
    removed     INTEGER DEFAULT 0,
    PRIMARY KEY (symbol, indexName, effectiveDate)
);
```
Required for `symbolsInUniverse()` to return correct per-universe symbol sets.

---

## 3. Minimum Historical Depth

| Use case | Fields | Depth required | Reason |
|---|---|---|---|
| Current point-in-time screen | All ratios | 1 latest quarter | Simple snapshot |
| `trend` / `consecutivePositive` operators | Any field | **5 quarters** | 5-period look-back |
| `epsGrowth5Y` | Annual EPS | **20 quarters (5Y)** | CAGR from 5 years ago |
| `peAvg5Y` / `yieldAvg5Y` | PE / yield | **20 quarters (5Y)** | Rolling average |
| `peAvg10Y` / `yieldAvg10Y` | PE / yield | **40 quarters (10Y)** | Rolling average |
| MA200 technical signal | Close price | **200 trading days** | ≈ 10 months of bars |
| Backtesting (future scope) | All bars | 10+ years | Strategy replay |

**Recommendation:** On first pipeline run, seed with **10 years of quarterly fundamentals** + **3 years of daily bars** for all S&P 500 and NASDAQ 100 symbols.

---

## 4. Per-Symbol Raw API Fields Summary

This is what the pipeline must request from an external API for each symbol, each quarter:

### From Income Statement (quarterly):
| Field | Used for |
|---|---|
| Revenue (TTM) | `ps`, `grossMargin`, `revenue` |
| Gross Profit (TTM) | `grossMargin` |
| Net Income (TTM) | `roe` |
| EPS Diluted (quarterly, for CAGR) | `epsGrowth5Y`, `peg` |
| Dividends Per Share (annualised) | `dividendYield` |

### From Balance Sheet (quarterly):
| Field | Used for |
|---|---|
| Total Shareholders' Equity | `roe`, `pb`, `debtEquity` |
| Total Debt (short-term + long-term) | `debtEquity` |
| Shares Outstanding (diluted) | `pb`, `ps`, `marketCap`, `sharesOutstanding` |

### From Cash Flow Statement (quarterly):
| Field | Used for |
|---|---|
| Operating Cash Flow | `fcf` |
| Capital Expenditure | `fcf` |

### From Market Data (daily close price on filing date):
| Field | Used for |
|---|---|
| Close price on `asOfDate` | `pe`, `pb`, `ps`, `dividendYield`, `marketCap` |

---

## 5. API Source Comparison

| API | OHLCV | Quarterly Financials | Historical depth | Free tier |
|---|---|---|---|---|
| **Yahoo Finance** (`yfinance`) | ✓ | ✓ snapshot + quarterly | 10+ years bars; 4–5 years financials | Unofficial; no rate limit |
| **Polygon.io** | ✓ | ✓ | Bars: 2003+; Financials: 2002+ | 5 calls/min |
| **Alpha Vantage** | ✓ | ✓ quarterly | 20+ years | 500 calls/day |
| **Finnhub** | ✓ | ✓ | 10+ years | 60 calls/min |

**Recommended combination (Phase 1):**

| Data | Recommended source | Reason |
|---|---|---|
| Daily OHLCV bars | `yfinance` | Free, easy, 10+ years |
| Quarterly EPS, revenue, FCF, equity, shares | `yfinance` (`quarterly_financials`, `quarterly_balance_sheet`, `quarterly_cashflow`) | All available in one library |
| 10-year historical quarterly data (for `peAvg10Y`) | Alpha Vantage `INCOME_STATEMENT` + `BALANCE_SHEET` | Goes back 20 years; free tier sufficient for bootstrap |
| Symbol master (name, exchange, sector) | `yfinance` `info` dict | Includes GICS sector |
| Index membership (SP500, NASDAQ100) | Wikipedia scrape or Polygon `/v3/reference/tickers` | Phase 2 |

---

## 6. Computation Order in the Pipeline

Run in this order for each ingestion cycle (daily after market close):

```
1. Fetch & store daily OHLCV bars for all symbols
        → INSERT INTO hourlyBars

2. On quarterly earnings release (or weekly refresh):
   a. Fetch raw financial statements (income, balance, cashflow)
   b. Compute: pe, pb, ps, peg, dividendYield, roe, grossMargin,
               debtEquity, fcf, marketCap, revenue, sharesOutstanding
   c. Compute: epsGrowth5Y  (requires 5Y of stored annual EPS)
   d. Compute: peAvg5Y, peAvg10Y  (requires looking back at own stored pe rows)
   e. Compute: yieldAvg5Y, yieldAvg10Y  (same)
        → INSERT INTO fundamentals (one row per symbol)

3. After ALL symbols for a given asOfDate are inserted:
   f. Compute cross-sectional: sectorAvgPe, sectorAvgPb, sectorMedianPs
        → UPDATE fundamentals WHERE asOfDate = <today's quarter end>

4. Refresh stocks master table for any new listings or sector reclassifications
        → UPSERT INTO stocks
```

---

## 7. Field × Spec Cross-Reference

| DB field | Where it's used |
|---|---|
| `pe`, `pb`, `ps`, `peg`, `dividendYield` | Spec_A (screener conditions), Spec_B (Models 1, 3, 4, 5, 6), Spec_D (bars) |
| `roe`, `grossMargin`, `debtEquity`, `fcf` | Spec_A (screener conditions), Spec_D (bars) |
| `epsGrowth5Y` | Spec_A (trend/peg conditions), Spec_B (Model 1 PEG, Model 2 DCF growth rate) |
| `marketCap`, `revenue` | Spec_A (results ranking), Spec_D (bars) |
| `sharesOutstanding` | Spec_B (Model 2 DCF — per-share intrinsic value calculation) |
| `peAvg5Y`, `peAvg10Y` | Spec_A (ratioVsAvg operator), Spec_B (Model 7 P/E vs Avg) |
| `yieldAvg5Y`, `yieldAvg10Y` | Spec_A (yieldAboveAvg operator), Spec_B (Model 8 Yield vs Avg) |
| `sectorAvgPe`, `sectorAvgPb` | Spec_A (FundamentalsRow), Spec_B (Models 3, 4 band calculation) |
| `sectorMedianPs` | Spec_B (Model 6 P/S vs Industry), Spec_D (bars) |
| `sector` | Spec_A (sector filter condition), Spec_B (display), Spec_D (bars) |
