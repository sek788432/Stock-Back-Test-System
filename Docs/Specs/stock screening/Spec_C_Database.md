# Spec C — Database Design

**Part of:** Stock Screener sub-specs  
**UI reference:** [`Screener_UI_Overview.md`](./Screener_UI_Overview.md)  
**Parent product spec:** [`11_Stock_Screener_KLine_Product.md`](../11_Stock_Screener_KLine_Product.md)  
**Constraint source:** [`04_Data_Layer.md`](../04_Data_Layer.md)  
**Referenced by:** `Spec_A_Screener_Engine.md`, `Spec_B_Valuation_Engine.md`, `Spec_D_NL_Python_Runtime.md`

---

## 1. Purpose

This spec defines every table the screener feature reads from or writes to.  
It is the **single source of truth** for column names, types, and constraints.  
All other specs reference this document — never define schema elsewhere.

---

## 2. Storage Architecture

Two completely separate databases with separate owners:

```
┌─────────────────────────────────────────────────┐
│  StockData/MarketData.duckdb                    │
│  Owner: Python pipeline (writes)                │
│  C++ app: READ-ONLY (Spec 04 hard rule)         │
│                                                 │
│  ┌─────────────┐  ┌──────────────┐  ┌────────┐ │
│  │ hourlyBars  │  │ fundamentals │  │ stocks │ │
│  │ (existing)  │  │  (NEW)       │  │ (NEW)  │ │
│  └─────────────┘  └──────────────┘  └────────┘ │
└─────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────┐
│  <userData>/app.db  (SQLite)                    │
│  Owner: C++ application (reads + writes)        │
│  Python: never touches this file                │
│                                                 │
│  ┌────────────────────┐  ┌──────────────────┐  │
│  │ screenerTemplates  │  │ screenerResults  │  │
│  └────────────────────┘  └──────────────────┘  │
│  ┌────────────────────┐  ┌──────────────────┐  │
│  │  valuationLists    │  │   nlAuditLog     │  │
│  └────────────────────┘  └──────────────────┘  │
│  ┌────────────────────┐                         │
│  │   schemaVersion    │  (migration tracking)   │
│  └────────────────────┘                         │
└─────────────────────────────────────────────────┘
```

**Why two separate databases?**

| Question | Answer |
|---|---|
| Why not put everything in DuckDB? | Spec 04: C++ is read-only on DuckDB. The app must write templates, results, and audit logs. |
| Why not put everything in SQLite? | Market data is owned by Python pipeline; mixing ownership causes write conflicts and upgrade pain. |
| Why SQLite for app.db? | Qt ships `QSqlDatabase` with a built-in SQLite driver — zero extra dependencies. |

---

## 3. MarketData.duckdb

All tables here are **written by Python, read by C++**. C++ never executes INSERT/UPDATE/DELETE.

---

### 3.1 `hourlyBars` — OHLCV Price Data (existing table)

> **Do not modify this table's existing columns.** The Python pipeline owns the schema.  
> C++ reads this via `BarStream` (Spec 04 §2). Do not query it directly in screener code.

```sql
-- Existing schema (from Spec 04 §1)
CREATE TABLE hourlyBars (
    symbol      TEXT        NOT NULL,
    ts          TIMESTAMPTZ NOT NULL,
    open        DOUBLE      NOT NULL,
    high        DOUBLE      NOT NULL,
    low         DOUBLE      NOT NULL,
    close       DOUBLE      NOT NULL,
    volume      BIGINT      NOT NULL,
    source      TEXT,
    schemaName  TEXT,           -- bar resolution: 'ohlcv-1d', 'ohlcv-1h', etc.
    ingestedAt  TIMESTAMPTZ
);
```

**Sample rows:**

```
symbol │ ts                     │ open  │ high  │ low   │ close │ volume   │ schemaName
───────┼────────────────────────┼───────┼───────┼───────┼───────┼──────────┼───────────
AAPL   │ 2026-05-28 13:30:00+00 │ 187.2 │ 188.5 │ 186.9 │ 188.1 │ 12340000 │ ohlcv-1d
AAPL   │ 2026-05-29 13:30:00+00 │ 188.3 │ 189.1 │ 187.8 │ 188.9 │ 10250000 │ ohlcv-1d
META   │ 2026-05-28 13:30:00+00 │ 594.1 │ 598.7 │ 592.3 │ 597.4 │  8910000 │ ohlcv-1d
META   │ 2026-05-29 13:30:00+00 │ 597.5 │ 602.1 │ 595.8 │ 601.2 │  9340000 │ ohlcv-1d
```

**Screener usage:** Python screen() script receives a slice of this table as the `bars` DataFrame (see `Spec_D` §2.2).

---

### 3.2 `fundamentals` — Per-Symbol Financial Metrics (new table)

This table does **not exist yet**. Python pipeline must add it. Each row = one symbol at one report date.

```sql
CREATE TABLE fundamentals (
    -- Identity
    symbol          TEXT    NOT NULL,
    asOfDate        DATE    NOT NULL,   -- fiscal quarter end date

    -- Valuation ratios  (Spec_B §2–7 reads these)
    pe              DOUBLE,             -- Price / Earnings (trailing)
    pb              DOUBLE,             -- Price / Book
    ps              DOUBLE,             -- Price / Sales
    peg             DOUBLE,             -- PEG Ratio  (pe / epsGrowth5Y * 100)
    dividendYield   DOUBLE,             -- annual dividend / price  (e.g. 0.052 = 5.2%)

    -- Quality metrics  (Spec_A screener conditions read these)
    roe             DOUBLE,             -- Return on Equity  (e.g. 0.348 = 34.8%)
    grossMargin     DOUBLE,             -- Gross Profit / Revenue  (e.g. 0.43 = 43%)
    debtEquity      DOUBLE,             -- Total Debt / Equity  (e.g. 0.5 = 50%)
    fcf             DOUBLE,             -- Free Cash Flow  (absolute USD, e.g. 5.2e10)
    epsGrowth5Y     DOUBLE,             -- EPS CAGR over 5 years  (e.g. 0.18 = 18%)

    -- DCF inputs  (Spec_B §3 reads these)
    marketCap         DOUBLE,           -- USD total market capitalisation
    revenue           DOUBLE,           -- trailing 12M revenue  (USD)
    sharesOutstanding DOUBLE,           -- diluted shares outstanding — required for per-share DCF

    -- Historical average context  (Spec_B §8-9 reads these)
    peAvg5Y         DOUBLE,             -- this symbol's own avg P/E over prior 5 years
    peAvg10Y        DOUBLE,             -- this symbol's own avg P/E over prior 10 years
    yieldAvg5Y      DOUBLE,             -- this symbol's own avg dividend yield over prior 5 years
    yieldAvg10Y     DOUBLE,             -- this symbol's own avg dividend yield over prior 10 years

    -- Cross-sectional context  (Spec_B §7 reads these)
    -- Pre-computed by Python pipeline after all symbols for the same asOfDate are inserted
    sector          TEXT,               -- e.g. 'Information Technology'
    sectorAvgPe     DOUBLE,             -- sector median P/E on this asOfDate
    sectorAvgPb     DOUBLE,             -- sector median P/B on this asOfDate
    sectorMedianPs  DOUBLE,             -- sector median P/S on this asOfDate

    -- Provenance
    source          TEXT,               -- e.g. 'yahoo_finance', 'polygon'
    ingestedAt      TIMESTAMPTZ DEFAULT now(),

    PRIMARY KEY (symbol, asOfDate)
);
```

**Sample rows:**

```
symbol │ asOfDate   │  roe  │    fcf    │   pe  │  pb  │  peg  │ dividendYield │ peAvg5Y │ peAvg10Y │ yieldAvg5Y │ yieldAvg10Y │ sector
───────┼────────────┼───────┼───────────┼───────┼──────┼───────┼───────────────┼─────────┼──────────┼────────────┼─────────────┼──────────────────────────
META   │ 2026-03-31 │ 0.348 │  5.20e+10 │ 22.40 │ 6.80 │ 0.820 │       0.00400 │   25.20 │    28.10 │      0.002 │       0.001 │ Communication Services
META   │ 2025-12-31 │ 0.331 │  4.90e+10 │ 20.10 │ 6.20 │ 0.770 │       0.00300 │   24.80 │    27.50 │      0.002 │       0.001 │ Communication Services
AAPL   │ 2026-03-31 │ 1.472 │  9.30e+10 │ 30.50 │ 48.2 │ 1.650 │       0.00600 │   27.30 │    22.10 │      0.008 │       0.009 │ Information Technology
O      │ 2026-03-31 │ 0.042 │  1.10e+09 │ 45.00 │ 1.20 │ 2.100 │       0.05800 │   48.20 │    52.30 │      0.049 │       0.046 │ Real Estate
```

**Reading rule for P/E vs Avg:**
- `pe < peAvg5Y` → current P/E is below the stock's own 5-year average → potentially cheap vs history → highlight
- `pe < peAvg10Y` → same logic for 10-year window

**Reading rule for Yield vs Avg (direction inverted vs P/E):**
- `dividendYield > yieldAvg5Y` → current yield is ABOVE average → price fell relative to dividend → potentially cheap → highlight
- A falling stock price raises the yield, so high yield vs history is a buy signal for income investors

**Key rule:** Always take the row with the **latest `asOfDate` ≤ screener's `asOfEnd`** for each symbol. This enforces no-lookahead.

```sql
-- Correct pattern for screener (no-lookahead)
SELECT DISTINCT ON (symbol) *
FROM fundamentals
WHERE asOfDate <= $asOfEnd
ORDER BY symbol, asOfDate DESC;
```

---

### 3.3 `stocks` — Symbol Master Data (new table)

Static reference data. Name and exchange rarely change; sector changes occasionally.

```sql
CREATE TABLE stocks (
    symbol      TEXT PRIMARY KEY,
    name        TEXT NOT NULL,          -- full company name
    exchange    TEXT,                   -- 'NASDAQ', 'NYSE'
    country     TEXT DEFAULT 'US',
    ingestedAt  TIMESTAMPTZ DEFAULT now()
);
```

**Sample rows:**

```
symbol │ name                     │ exchange │ country
───────┼──────────────────────────┼──────────┼────────
META   │ Meta Platforms Inc.      │ NASDAQ   │ US
AAPL   │ Apple Inc.               │ NASDAQ   │ US
O      │ Realty Income Corp.      │ NYSE     │ US
NVDA   │ NVIDIA Corporation       │ NASDAQ   │ US
MSFT   │ Microsoft Corporation    │ NASDAQ   │ US
JNJ    │ Johnson & Johnson        │ NYSE     │ US
```

**Screener usage:** Joined to produce the Results Table `Company Name` column (Spec A §4, Spec UI §A6).

---

### 3.4 `indexConstituents` — Universe Membership (Phase 2, not yet created)

> **Phase 1:** `Spec_A::symbolsInUniverse()` returns all symbols with fundamentals data. The `universe` parameter is ignored.  
> **Phase 2:** Add this table. Python pipeline writes it; C++ reads it to filter by named universe.

```sql
-- Phase 2: not yet created. Python pipeline owns writes; C++ reads only.
CREATE TABLE indexConstituents (
    symbol        TEXT    NOT NULL,
    indexName     TEXT    NOT NULL,   -- e.g. 'SP500', 'NASDAQ100', 'NYSE'
    effectiveDate DATE    NOT NULL,   -- date the symbol joined the index
    removedDate   DATE,               -- NULL = still in index; set when removed
    ingestedAt    TIMESTAMPTZ DEFAULT now(),

    PRIMARY KEY (symbol, indexName, effectiveDate)
);
```

**Phase 2 query in `symbolsInUniverse()`:**
```sql
SELECT DISTINCT symbol
FROM indexConstituents
WHERE indexName     = ?           -- universe param
  AND effectiveDate <= ?          -- no-lookahead: only members as of asOfEnd
  AND (removedDate IS NULL OR removedDate > ?)   -- still in index at asOfEnd
ORDER BY symbol;
```

**Sample rows (after Phase 2 pipeline runs):**

```
symbol │ indexName │ effectiveDate │ removedDate
───────┼───────────┼───────────────┼────────────
META   │ SP500     │ 2013-12-23    │ NULL
AAPL   │ SP500     │ 1982-11-30    │ NULL
AAPL   │ NASDAQ100 │ 1998-12-31    │ NULL
TSLA   │ SP500     │ 2020-12-21    │ NULL
```

---

### 3.5 MarketData.duckdb — Indexes

```sql
-- hourlyBars: primary access pattern is (symbol, schemaName, ts range)
CREATE INDEX idx_hourlyBars_sym_schema_ts
    ON hourlyBars (symbol, schemaName, ts);

-- fundamentals: primary pattern is (symbol, asOfDate DESC)
CREATE INDEX idx_fundamentals_sym_date
    ON fundamentals (symbol, asOfDate DESC);

-- fundamentals: sector cross-sectional queries
CREATE INDEX idx_fundamentals_sector_date
    ON fundamentals (sector, asOfDate);
```

---

## 4. app.db (SQLite)

All tables here are **owned by the C++ application**. Python never reads or writes this file.  
Location: `<userData>/screener/app.db`

---

### 4.1 `schemaVersion` — Migration Tracking

Always the first table created. Used for safe schema upgrades.

```sql
CREATE TABLE IF NOT EXISTS schemaVersion (
    version     INTEGER NOT NULL,
    appliedAt   DATETIME DEFAULT CURRENT_TIMESTAMP,
    description TEXT
);

-- Seed row on first launch
INSERT INTO schemaVersion (version, description)
VALUES (1, 'Initial screener schema');
```

**Sample rows:**

```
version │ appliedAt           │ description
────────┼─────────────────────┼───────────────────────────────
1       │ 2026-05-30 14:00:00 │ Initial screener schema
2       │ 2026-06-15 09:30:00 │ Add valuationLists.pinned column
```

---

### 4.2 `screenerTemplates` — Saved Condition Sets

One row = one saved screener configuration (from any of the three modes).

```sql
CREATE TABLE screenerTemplates (
    id          INTEGER  PRIMARY KEY AUTOINCREMENT,
    name        TEXT     NOT NULL,
    mode        TEXT     NOT NULL CHECK (mode IN ('builtin', 'python', 'nl')),

    -- builtin mode only
    logic       TEXT     CHECK (logic IN ('AND', 'OR')),
    conditions  TEXT,    -- JSON array of condition blocks (see §4.2.1)

    -- python / nl mode only
    scriptCode  TEXT,    -- full Python source accepted by user

    -- nl mode extra audit link
    nlAuditId   INTEGER  REFERENCES nlAuditLog(id),

    createdAt   DATETIME DEFAULT CURRENT_TIMESTAMP,
    updatedAt   DATETIME DEFAULT CURRENT_TIMESTAMP
);
```

#### 4.2.1 `conditions` JSON format

```json
[
  { "category": "Moat",      "field": "roe",          "operator": ">=", "value": 0.15  },
  { "category": "Moat",      "field": "grossMargin",  "operator": ">=", "value": 0.30  },
  { "category": "Leverage",  "field": "debtEquity",   "operator": "<=", "value": 0.50  },
  { "category": "Liquidity", "field": "fcf",          "operator": ">",  "value": 0     },
  { "category": "Technical", "field": "priceVsMa200", "operator": "==", "value": "ABOVE" }
]
```

**Sample rows:**

```
id │ name             │ mode    │ logic │ conditions (abbreviated)                       │ scriptCode
───┼──────────────────┼─────────┼───────┼────────────────────────────────────────────────┼───────────
1  │ Buffett Value    │ builtin │ AND   │ [{"field":"roe","op":">=","val":0.15}, ...]     │ NULL
2  │ Momentum Breakout│ builtin │ AND   │ [{"field":"volume_ratio","op":">=","val":2.5}]  │ NULL
3  │ My Python Screen │ python  │ NULL  │ NULL                                            │ def screen(...): ...
4  │ AI Growth 2026   │ nl      │ NULL  │ NULL                                            │ def screen(...): ...
```

---

### 4.3 `screenerResults` — Run History

One row = one completed Run Screen execution. Stores which symbols passed.

```sql
CREATE TABLE screenerResults (
    id          INTEGER  PRIMARY KEY AUTOINCREMENT,
    name        TEXT     NOT NULL,              -- user-given label on save

    -- what was run
    templateId  INTEGER  REFERENCES screenerTemplates(id),
    asOfStart   DATE     NOT NULL,
    asOfEnd     DATE     NOT NULL,
    universe    TEXT     NOT NULL,              -- 'SP500', 'NASDAQ100', 'CUSTOM'
    logic       TEXT,                           -- 'AND' or 'OR' (snapshot at run time)

    -- what came out
    symbols     TEXT     NOT NULL,              -- JSON array: ["META","NVDA","MSFT"]
    rowCount    INTEGER  NOT NULL,

    runAt       DATETIME DEFAULT CURRENT_TIMESTAMP
);
```

**Sample rows:**

```
id │ name                │ templateId │ asOfStart  │ asOfEnd    │ universe │ symbols                    │ rowCount
───┼─────────────────────┼────────────┼────────────┼────────────┼──────────┼────────────────────────────┼─────────
1  │ Buffett_2026_Q1     │ 1          │ 2025-01-01 │ 2026-03-31 │ SP500    │ ["META","MSFT","JNJ"]      │ 3
2  │ Momentum_May2026    │ 2          │ 2025-06-01 │ 2026-05-30 │ NASDAQ100│ ["NVDA","TSLA"]            │ 2
3  │ My_Custom_Run       │ 3          │ 2024-01-01 │ 2026-05-30 │ SP500    │ ["META","NVDA","MSFT","O"] │ 4
```

---

### 4.4 `valuationLists` — Block B Active Lists

One row = one named list loaded into the Valuation Matrix.

```sql
CREATE TABLE valuationLists (
    id              INTEGER  PRIMARY KEY AUTOINCREMENT,
    name            TEXT     NOT NULL,

    -- origin (at least one must be non-null)
    sourceResultId  INTEGER  REFERENCES screenerResults(id),  -- came from Screener
    symbols         TEXT     NOT NULL,  -- JSON array (current state, may differ from source)

    -- thresholds snapshot (what was active when list was last viewed)
    thresholdPeg    REAL     DEFAULT 1.0,
    thresholdDcfMos REAL     DEFAULT 20.0,
    thresholdPeBand TEXT     DEFAULT 'MID_LOW',
    thresholdPbBand TEXT     DEFAULT 'LOW_TRACK',
    thresholdDdm    REAL     DEFAULT 4.5,
    thresholdPs     TEXT     DEFAULT 'UNDERVALUED',

    createdAt       DATETIME DEFAULT CURRENT_TIMESTAMP,
    updatedAt       DATETIME DEFAULT CURRENT_TIMESTAMP
);
```

**Sample rows:**

```
id │ name                  │ sourceResultId │ symbols                       │ thresholdPeg │ thresholdDcfMos
───┼───────────────────────┼────────────────┼───────────────────────────────┼──────────────┼────────────────
1  │ Default: Core Tech    │ NULL           │ ["META","AAPL","O"]           │ 1.0          │ 20.0
2  │ DB: Buffett_2026_Q1   │ 1              │ ["META","MSFT","JNJ"]         │ 1.0          │ 20.0
3  │ DB: Momentum_May2026  │ 2              │ ["NVDA","TSLA","META"]        │ 1.5          │ 15.0
```

---

### 4.5 `nlAuditLog` — AI Prompt Audit Trail

One row = one AI generation turn. Required by Spec 11 §3.2 for litigation-grade traceability.

```sql
CREATE TABLE nlAuditLog (
    id              INTEGER  PRIMARY KEY AUTOINCREMENT,
    prompt          TEXT     NOT NULL,          -- exact user input
    modelId         TEXT     NOT NULL,          -- e.g. 'claude-sonnet-4-6'
    modelVersion    TEXT,                       -- build/version string if available
    sourceHash      TEXT     NOT NULL,          -- sha256(generatedCode) hex string
    generatedCode   TEXT     NOT NULL,          -- full Python source as generated
    accepted        INTEGER  NOT NULL DEFAULT 0,-- 0 = rejected / pending, 1 = accepted
    acceptedAt      DATETIME,                   -- NULL if not accepted
    templateId      INTEGER  REFERENCES screenerTemplates(id), -- set on Accept
    resultId        INTEGER  REFERENCES screenerResults(id)    -- set after Run Screen
);
```

**Sample rows:**

```
id │ prompt                               │ modelId           │ sourceHash │ accepted │ acceptedAt          │ templateId
───┼──────────────────────────────────────┼───────────────────┼────────────┼──────────┼─────────────────────┼───────────
1  │ Find stocks with ROE > 15%, positive │ claude-sonnet-4-6 │ a3f92e1c   │ 1        │ 2026-05-30 14:20:05 │ 4
2  │ Add momentum filter on top           │ claude-sonnet-4-6 │ b7c41d9e   │ 0        │ NULL                │ NULL
3  │ Only include dividend stocks > 4%    │ claude-sonnet-4-6 │ c8e53f2a   │ 1        │ 2026-05-30 15:10:44 │ 5
```

---

### 4.6 app.db — Indexes

```sql
-- Most common query: load templates list sorted by recent
CREATE INDEX idx_templates_createdAt
    ON screenerTemplates (createdAt DESC);

-- Results lookup by template
CREATE INDEX idx_results_templateId
    ON screenerResults (templateId);

-- Audit log: find entries by acceptance status
CREATE INDEX idx_audit_accepted
    ON nlAuditLog (accepted, acceptedAt DESC);

-- Valuation lists: find lists sourced from a result
CREATE INDEX idx_valLists_sourceResult
    ON valuationLists (sourceResultId);
```

---

## 5. Column Naming Convention

Follows project-wide rules from `Docs/Specs/README.md`:

| Rule | Example |
|---|---|
| Column names: `lowerCamelCase` | `asOfDate`, `sourceHash`, `marketCap` |
| Table names: `lowerCamelCase` (plural) | `screenerTemplates`, `nlAuditLog` |
| Boolean stored as INTEGER in SQLite | `accepted INTEGER` (0 / 1) |
| Monetary values: raw USD DOUBLE | `fcf DOUBLE` (not formatted strings) |
| Ratios: decimal form | `roe = 0.15` means 15%, not `15` |
| Dates: `DATE` (no time) or `DATETIME` (with time) | `asOfDate DATE`, `createdAt DATETIME` |

---

## 6. User Workflow Examples

This section shows exactly how the database changes at every step of the user journey.

---

### Example 1: User loads the "Buffett Value" preset and clicks Save as Template

**User action:** Built-in mode → loads Buffett Value preset → clicks "Save as Template" → types `"My Buffett Screen"`

**DB change:** One INSERT into `screenerTemplates`

```sql
INSERT INTO screenerTemplates (name, mode, logic, conditions)
VALUES (
    'My Buffett Screen',
    'builtin',
    'AND',
    '[
        {"category":"Stability", "field":"epsGrowth5Y",  "operator":"trend", "value":"stable"},
        {"category":"Moat",      "field":"roe",          "operator":">=",    "value":0.15},
        {"category":"Moat",      "field":"grossMargin",  "operator":">=",    "value":0.30},
        {"category":"Leverage",  "field":"debtEquity",   "operator":"<=",    "value":0.50},
        {"category":"Liquidity", "field":"fcf",          "operator":">",     "value":0}
    ]'
);
-- → id = 5, createdAt = now()
```

**State after:**

```
screenerTemplates
id=5 │ name='My Buffett Screen' │ mode='builtin' │ logic='AND' │ conditions=[5 blocks]
```

---

### Example 2: User runs the screen and saves the result

**User action:** Sets universe = S&P 500, dates 2025-01-01 → 2026-05-30 → clicks "Run Screen" → 3 matches → clicks "Save List to Database" → types `"Buffett_SP500_May2026"`

**Step 1 — Engine evaluates conditions** (no DB write yet, pure computation)

**Step 2 — INSERT into `screenerResults`**

```sql
INSERT INTO screenerResults (name, templateId, asOfStart, asOfEnd, universe, logic, symbols, rowCount)
VALUES (
    'Buffett_SP500_May2026',
    5,                          -- templateId from Example 1
    '2025-01-01',
    '2026-05-30',
    'SP500',
    'AND',
    '["META", "MSFT", "JNJ"]',
    3
);
-- → id = 4, runAt = now()
```

**Step 3 — New entry appears in Block B Source A dropdown**

```sql
-- App reads this to populate the dropdown
SELECT id, name, rowCount FROM screenerResults ORDER BY runAt DESC;
```

```
id │ name                     │ rowCount
───┼──────────────────────────┼─────────
4  │ Buffett_SP500_May2026    │ 3        ← newly appears in dropdown
3  │ My_Custom_Run            │ 4
2  │ Momentum_May2026         │ 2
1  │ Buffett_2026_Q1          │ 3
```

---

### Example 3: User uses NL mode — sends a prompt, AI generates code, user accepts

**User action:** NL tab → types prompt → clicks Send → AI responds → clicks "Accept & Compile"

**Step A — User clicks Send (AI generates, not yet accepted)**

```sql
INSERT INTO nlAuditLog (prompt, modelId, modelVersion, sourceHash, generatedCode, accepted)
VALUES (
    'Find profitable tech stocks with strong cash flow and below-market P/E',
    'claude-sonnet-4-6',
    'build-2026-05',
    'f4a92b3c8d1e7f06a5b4c3d2e1f09876',  -- sha256 of the generated code
    'def screen(symbol: str, bars, as_of) -> bool:
    roe        = bars[''roe''].iloc[-1]
    fcf        = bars[''fcf''].iloc[-1]
    pe         = bars[''pe''].iloc[-1]
    sector_pe  = bars[''sectorAvgPe''].iloc[-1]
    return roe >= 0.15 and fcf > 0 and pe < sector_pe',
    0           -- not accepted yet
);
-- → id = 4
```

**State after Send (before Accept):**

```
nlAuditLog
id=4 │ accepted=0 │ acceptedAt=NULL │ templateId=NULL │ resultId=NULL
```

**Step B — User clicks "Accept & Compile"**

```sql
-- 1. Mark the audit entry as accepted
UPDATE nlAuditLog
SET    accepted = 1, acceptedAt = CURRENT_TIMESTAMP
WHERE  id = 4;

-- 2. Save as a new template
INSERT INTO screenerTemplates (name, mode, scriptCode, nlAuditId)
VALUES ('AI: tech cash flow screen', 'nl', 'def screen(...): ...', 4);
-- → templateId = 6

-- 3. Link the audit record back to the template
UPDATE nlAuditLog SET templateId = 6 WHERE id = 4;
```

**State after Accept:**

```
nlAuditLog
id=4 │ accepted=1 │ acceptedAt=2026-05-30 15:10:44 │ templateId=6 │ resultId=NULL

screenerTemplates
id=6 │ name='AI: tech cash flow screen' │ mode='nl' │ scriptCode='def screen...' │ nlAuditId=4
```

---

### Example 4: User loads a saved list into Block B Valuation Matrix

**User action:** Block B → Source A dropdown → selects "Buffett_SP500_May2026"

**App reads:**

```sql
-- 1. Get the symbol list
SELECT symbols FROM screenerResults WHERE id = 4;
-- → ["META", "MSFT", "JNJ"]

-- 2. Get latest fundamentals for each symbol (no-lookahead)
SELECT DISTINCT ON (symbol)
    f.symbol, f.pe, f.pb, f.peg, f.dividendYield,
    f.roe, f.fcf, f.marketCap, f.sectorAvgPe,
    s.name, s.sector
FROM fundamentals f
JOIN stocks s USING (symbol)
WHERE f.symbol IN ('META', 'MSFT', 'JNJ')
  AND f.asOfDate <= '2026-05-30'      -- asOfEnd from the result
ORDER BY f.symbol, f.asOfDate DESC;

-- 3. Get latest price for each symbol
SELECT symbol, close AS lastPrice
FROM hourlyBars
WHERE symbol IN ('META', 'MSFT', 'JNJ')
  AND schemaName = 'ohlcv-1d'
  AND ts <= '2026-05-30 23:59:59+00'
QUALIFY ROW_NUMBER() OVER (PARTITION BY symbol ORDER BY ts DESC) = 1;
```

**No DB write on this step** — just reads.

**If user clicks "+ Add" to manually append NVDA:**

```sql
-- App updates the in-memory active list (does not persist until user saves)
-- If user saves the modified list:
INSERT INTO valuationLists (name, sourceResultId, symbols)
VALUES (
    'DB: Buffett_SP500_May2026',
    4,
    '["META", "MSFT", "JNJ", "NVDA"]'   -- NVDA appended
);
-- → id = 4
```

---

### Example 5: User adjusts Valuation Highlight Thresholds

**User action:** Changes "DCF Margin of Safety" from 20% to 30% by typing in the input box

**DB change:** None. Thresholds only trigger a **frontend re-render** — the Composite Score recalculates in memory. Thresholds are only persisted when the user explicitly saves a `valuationLists` row.

```
No SQL executed. Pure in-memory recalculation.
```

---

### Example 6: User exports Screener results to CSV

**User action:** Clicks "Export CSV" after a run

**App reads (same as display, no write):**

```sql
SELECT
    ROW_NUMBER() OVER (ORDER BY symbol) AS rank,
    s.symbol,
    s.name,
    h.close                              AS lastPrice,
    -- change % computed from last 2 bars
    (h.close - h2.close) / h2.close     AS changePercent,
    f.marketCap,
    f.sector
FROM screenerResults r
CROSS JOIN JSON_EACH(r.symbols) j
JOIN stocks       s ON s.symbol = j.value
JOIN fundamentals f ON f.symbol = j.value AND f.asOfDate = (
        SELECT MAX(asOfDate) FROM fundamentals
        WHERE symbol = j.value AND asOfDate <= r.asOfEnd)
JOIN hourlyBars   h  ON h.symbol  = j.value AND h.schemaName = 'ohlcv-1d'
                    AND h.ts = (SELECT MAX(ts) FROM hourlyBars WHERE symbol = j.value
                                AND schemaName = 'ohlcv-1d' AND ts::DATE <= r.asOfEnd)
JOIN hourlyBars   h2 ON h2.symbol = j.value AND h2.schemaName = 'ohlcv-1d'
                    AND h2.ts = (SELECT MAX(ts) FROM hourlyBars WHERE symbol = j.value
                                AND schemaName = 'ohlcv-1d'
                                AND ts::DATE < h.ts::DATE)
WHERE r.id = 4;
```

**CSV output written to disk, no app.db change.**

---

### Example 7: User rejects an AI suggestion then refines and accepts a second version

**User action:** NL mode → sends prompt → rejects AI response → sends follow-up → accepts second response

**Step 1 — First generation (rejected):**

```sql
INSERT INTO nlAuditLog (prompt, modelId, sourceHash, generatedCode, accepted)
VALUES ('Find dividend stocks above 4%', 'claude-sonnet-4-6', 'aaa111bbb', 'def screen...(v1)', 0);
-- id = 5, accepted = 0
```

**Step 2 — Reject (no SQL needed — rejection is implied by accepted = 0 and no acceptedAt):**

The row stays as-is. The `accepted = 0, acceptedAt = NULL` combination means rejected.

**Step 3 — Second generation:**

```sql
INSERT INTO nlAuditLog (prompt, modelId, sourceHash, generatedCode, accepted)
VALUES (
    'Find dividend stocks above 4%, also exclude REITs',   -- refined prompt
    'claude-sonnet-4-6',
    'ccc333ddd',
    'def screen...(v2)',
    0
);
-- id = 6
```

**Step 4 — Accept second version:**

```sql
UPDATE nlAuditLog SET accepted = 1, acceptedAt = CURRENT_TIMESTAMP WHERE id = 6;

INSERT INTO screenerTemplates (name, mode, scriptCode, nlAuditId)
VALUES ('AI: dividend ex-REIT', 'nl', 'def screen...(v2)', 6);
```

**Final state — both audit rows preserved:**

```
nlAuditLog
id=5 │ prompt='Find dividend stocks...'          │ accepted=0 │ acceptedAt=NULL  ← rejected, kept for audit
id=6 │ prompt='Find dividend stocks...no REIT'   │ accepted=1 │ acceptedAt=...   ← accepted
```

Both rows stay forever. Deletion is not permitted (audit integrity).

---

## 7. Cross-Reference Index

| Spec | Tables it reads | Tables it writes |
|---|---|---|
| **Spec_A** (Screener Engine) | `fundamentals`, `stocks`, `hourlyBars` | `screenerTemplates`, `screenerResults` |
| **Spec_B** (Valuation Engine) | `fundamentals`, `hourlyBars`, `stocks`, `valuationLists` | `valuationLists` (on save) |
| **Spec_D** (NL / Python Runtime) | `hourlyBars`, `fundamentals` (as DataFrame) | `nlAuditLog`, `screenerTemplates` |
| **Python pipeline** | — | `hourlyBars`, `fundamentals` (all columns including `sharesOutstanding`, `sectorAvgPe/Pb`, `sectorMedianPs`, `peAvg5Y/10Y`, `yieldAvg5Y/10Y`), `stocks` |
| **C++ app.db init** | `schemaVersion` | `schemaVersion` (on migration) |

---

## 8. Data Flow Diagrams

---

### 8.1 System Overview — Who Owns What

```
  EXTERNAL DATA SOURCES              MarketData.duckdb
  ─────────────────────              ──────────────────────────────────────────────
  ┌─────────────────┐                ┌──────────────────────────────────────────┐
  │  Yahoo Finance  │                │                                          │
  │  Polygon.io     │ ──── write ──► │  ┌─────────────┐   ┌──────────────────┐ │
  │  (etc.)         │                │  │  hourlyBars │   │   fundamentals   │ │
  └─────────────────┘                │  │             │   │                  │ │
                                     │  │ symbol      │   │ symbol           │ │
  ┌─────────────────┐                │  │ ts          │   │ asOfDate         │ │
  │ Python Pipeline │ ──── write ──► │  │ open/high/  │   │ roe, fcf, pe, pb │ │
  │ (DataFetcher)   │                │  │ low/close/  │   │ peAvg5Y/10Y      │ │
  │                 │                │  │ volume      │   │ yieldAvg5Y/10Y   │ │
  │  runs daily     │                │  │ schemaName  │   │ sectorAvgPe/Pb   │ │
  │  after market   │                │  └─────────────┘   └──────────────────┘ │
  └─────────────────┘                │                                          │
                                     │  ┌─────────────┐                         │
         C++ NEVER WRITES ─────────► │  │   stocks    │  READ-ONLY from C++    │
         THIS FILE (Spec 04)         │  │             │                         │
                                     │  │ symbol      │                         │
                                     │  │ name        │                         │
                                     │  │ exchange    │                         │
                                     │  └─────────────┘                         │
                                     └──────────────────────────────────────────┘
                                                    │
                                                    │  READ ONLY
                                                    ▼
                              ┌─────────────────────────────────────────────┐
                              │              C++ Application                  │
                              │                                               │
                              │  ┌─────────────────────────────────────────┐ │
                              │  │            Block A — Screener            │ │
                              │  │                                          │ │
                              │  │  reads ◄── fundamentals (conditions)    │ │
                              │  │  reads ◄── stocks      (name/sector)    │ │
                              │  │  reads ◄── hourlyBars  (Python/NL bars) │ │
                              │  │                                          │ │
                              │  │  writes ──► screenerTemplates           │ │
                              │  │  writes ──► screenerResults             │ │
                              │  │  writes ──► nlAuditLog                  │ │
                              │  └─────────────────────────────────────────┘ │
                              │                                               │
                              │  ┌─────────────────────────────────────────┐ │
                              │  │            Block B — Valuation           │ │
                              │  │                                          │ │
                              │  │  reads ◄── fundamentals (ratios/avgs)   │ │
                              │  │  reads ◄── hourlyBars  (price/MA)       │ │
                              │  │  reads ◄── stocks      (name/sector)    │ │
                              │  │  reads ◄── screenerResults (Source A)   │ │
                              │  │  reads ◄── valuationLists               │ │
                              │  │                                          │ │
                              │  │  writes ──► valuationLists  (on save)   │ │
                              │  └─────────────────────────────────────────┘ │
                              └─────────────────────────────────────────────┘
                                                    │
                                                    │  READ / WRITE
                                                    ▼
                              ┌─────────────────────────────────────────────┐
                              │           app.db  (SQLite)                    │
                              │           C++ owns this file entirely          │
                              │                                               │
                              │  ┌──────────────────┐  ┌──────────────────┐  │
                              │  │screenerTemplates  │  │ screenerResults  │  │
                              │  │(saved conditions) │  │ (run history)    │  │
                              │  └──────────────────┘  └──────────────────┘  │
                              │  ┌──────────────────┐  ┌──────────────────┐  │
                              │  │ valuationLists    │  │   nlAuditLog     │  │
                              │  │ (Block B lists)   │  │ (AI audit trail) │  │
                              │  └──────────────────┘  └──────────────────┘  │
                              └─────────────────────────────────────────────┘
```

---

### 8.2 Block A — Screener Execution Flow

```
  USER ACTION                  READS FROM                   WRITES TO
  ───────────                  ──────────                   ─────────

  ┌─────────────────────┐
  │  User configures    │
  │  conditions         │
  │  (Built-in / Python │
  │   / NL mode)        │
  └──────────┬──────────┘
             │
             │ Save Template
             ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │  INSERT screenerTemplates                                         │
  │  { name, mode, logic, conditions / scriptCode, nlAuditId }       │
  └──────────────────────────────────────────────────────────────────┘
             │
             │ Click "Run Screen"
             ▼
  ┌─────────────────────┐     ┌──────────────────────────────────────┐
  │   Mode = builtin    │────►│ SELECT fundamentals                   │
  │                     │     │   WHERE asOfDate <= asOfEnd  ← no     │
  │   Evaluate each     │     │   ORDER BY symbol, asOfDate DESC       │
  │   condition block   │     │                                        │
  │   (AND / OR)        │     │ SELECT stocks (name, sector)           │
  └─────────────────────┘     └──────────────────────────────────────┘

  ┌─────────────────────┐     ┌──────────────────────────────────────┐
  │   Mode = python     │────►│ SELECT hourlyBars (build bars DF)     │
  │   or nl             │     │ JOIN   fundamentals (pe, roe, fcf...) │
  │                     │     │ JOIN   fundamentals (peAvg5Y, etc.)   │
  │   Run screen()      │     │ → pass to Python sandbox              │
  │   in sandbox        │     └──────────────────────────────────────┘
  └─────────────────────┘

             │
             │ Evaluation complete
             ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │  Results in memory: ["META", "NVDA", "MSFT"]                      │
  │  Display in Results Table (Rank/Symbol/Name/Price/Chg%/Cap/Sector)│
  └──────────────────────────────────────────────────────────────────┘
             │
             │ User clicks "Save List to Database"
             ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │  INSERT screenerResults                                           │
  │  { name, templateId, asOfStart, asOfEnd, universe,               │
  │    symbols (JSON), rowCount }                                     │
  │                                                                   │
  │  → This row now appears in Block B Source A dropdown             │
  └──────────────────────────────────────────────────────────────────┘
```

---

### 8.3 Block B — Valuation Load Flow

```
  USER ACTION                  READS FROM                   WRITES TO
  ───────────                  ──────────                   ─────────

  Three input paths feed the Active Valuation List:

  ┌────────────────────┐
  │ Source A           │──► SELECT symbols FROM screenerResults WHERE id = ?
  │ (load from DB)     │    → symbols = ["META","NVDA","MSFT"]
  └────────────────────┘
  ┌────────────────────┐
  │ Source B           │──► User types "JNJ" → append to active list
  │ (manual ticker)    │
  └────────────────────┘
  ┌────────────────────┐
  │ Source C           │──► Parse CSV/Excel → extract symbol column
  │ (import file)      │    → append to active list
  └────────────────────┘
             │
             │ Active list assembled: ["META","NVDA","MSFT","JNJ"]
             ▼
  For EACH symbol in active list:
  ┌──────────────────────────────────────────────────────────────────┐
  │  SELECT fundamentals   ← pe, pb, peg, dividendYield, roe, fcf   │
  │                           peAvg5Y, peAvg10Y                       │
  │                           yieldAvg5Y, yieldAvg10Y                 │
  │                           marketCap, sectorAvgPe                  │
  │  WHERE asOfDate <= asOfEnd  (no-lookahead)                        │
  │                                                                   │
  │  SELECT hourlyBars     ← close (lastPrice), volume               │
  │                           ma5, ma20, ma60 (Technical Signal)      │
  │  WHERE ts = latest bar <= asOfEnd                                 │
  │                                                                   │
  │  SELECT stocks         ← name, sector                             │
  └──────────────────────────────────────────────────────────────────┘
             │
             │ Compute in memory:
             │   Composite Score (using 8 model checks × +10 pts)
             │   Technical Signal (bullish / neutral / bearish)
             ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │  Display Valuation Table                                          │
  │  Ticker│Score│PEG│DCF│PE Band│PB Band│Yield│PS│PE vs Avg│Yield vs Avg│Tech│
  └──────────────────────────────────────────────────────────────────┘
             │
             │ User clicks "+ Add" on screener result row
             │ or adjusts threshold controls → no DB write, in-memory recalc
             │
             │ User saves list
             ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │  INSERT valuationLists                                            │
  │  { name, sourceResultId, symbols (JSON),                         │
  │    thresholdPeg, thresholdDcfMos, thresholdPeBand, ... }         │
  └──────────────────────────────────────────────────────────────────┘
```

---

### 8.4 NL Mode — AI Prompt to Accepted Template

```
  USER ACTION                        DB OPERATIONS
  ───────────                        ─────────────

  User types prompt
  "Find profitable tech stocks..."
             │
             │ Click Send
             ▼
  C++ calls Claude API ─────────────── (network, no DB yet)
             │
             │ API responds with generated Python code
             ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │  INSERT nlAuditLog                                                │
  │  { prompt, modelId='claude-sonnet-4-6',                          │
  │    sourceHash=sha256(code), generatedCode, accepted=0 }          │
  │                                               ↑                  │
  │                              not accepted yet │ audit always kept │
  └──────────────────────────────────────────────────────────────────┘
             │
             │ Display to user in Chat bubble
             │
             ├─────────────────────────────────────────────────────┐
             │  User clicks REJECT                                  │
             │                                                      │
             │  nlAuditLog stays (accepted=0, acceptedAt=NULL)      │
             │  ← permanent audit record of the rejection           │
             │                                                      │
             │  User types new/refined prompt → loop back to Send   │
             └─────────────────────────────────────────────────────┘
             │
             │  User clicks ACCEPT
             ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │  UPDATE nlAuditLog                                                │
  │  SET accepted=1, acceptedAt=now()                                │
  │  WHERE id = ?                                                     │
  └──────────────────────────────────────────────────────────────────┘
             │
             ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │  INSERT screenerTemplates                                         │
  │  { name, mode='nl', scriptCode, nlAuditId=? }                    │
  └──────────────────────────────────────────────────────────────────┘
             │
             ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │  UPDATE nlAuditLog SET templateId=?   ← link back to template    │
  └──────────────────────────────────────────────────────────────────┘
             │
             │ User runs screen (same as 8.2 Python mode)
             ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │  INSERT screenerResults  { ... }                                  │
  │  UPDATE nlAuditLog SET resultId=?     ← full audit chain complete │
  └──────────────────────────────────────────────────────────────────┘

  Final audit chain:
  nlAuditLog.id ──► nlAuditLog.templateId ──► screenerTemplates.id
                                          ──► screenerTemplates.nlAuditId
  nlAuditLog.id ──► nlAuditLog.resultId   ──► screenerResults.id
```

---

### 8.5 app.db Entity Relationships

```
  ┌─────────────────────────────────────────────────────────────────┐
  │  app.db — Table Relationships                                     │
  └─────────────────────────────────────────────────────────────────┘

  ┌──────────────────────────┐        ┌──────────────────────────────┐
  │     nlAuditLog           │        │      screenerTemplates        │
  │  ───────────────         │        │  ────────────────────         │
  │  id          (PK)        │◄───────│  nlAuditId  (FK, nullable)   │
  │  prompt                  │        │  ───────────────────────────  │
  │  modelId                 │        │  id          (PK)             │
  │  modelVersion            │        │  name                         │
  │  sourceHash              │        │  mode  (builtin/python/nl)    │
  │  generatedCode           │        │  logic (AND/OR)               │
  │  accepted  (0/1)         │        │  conditions  (JSON)           │
  │  acceptedAt              │        │  scriptCode                   │
  │  templateId (FK) ────────┼───────►│  createdAt                    │
  │  resultId   (FK) ──┐     │        └──────────────────────────────┘
  └──────────────────┬─┘     │                      │
                     │       └──────────────────────┘      1
                     │                                      │
                     │                                      │ templateId (FK)
                     │                                      │
                     │                                      │ n
                     │              ┌──────────────────────────────────┐
                     └─────────────►│        screenerResults            │
                       resultId FK  │  ──────────────────────────────   │
                                    │  id          (PK)                 │
                                    │  name                             │
                                    │  templateId  (FK)                 │
                                    │  asOfStart                        │
                                    │  asOfEnd                          │
                                    │  universe                         │
                                    │  symbols     (JSON array)         │
                                    │  rowCount                         │
                                    │  runAt                            │
                                    └──────────────────────────────────┘
                                                   │
                                                   │ 1
                                                   │ sourceResultId (FK, nullable)
                                                   │ n
                                    ┌──────────────────────────────────┐
                                    │         valuationLists            │
                                    │  ──────────────────────────────   │
                                    │  id              (PK)             │
                                    │  name                             │
                                    │  sourceResultId  (FK, nullable)   │
                                    │  symbols         (JSON array)     │
                                    │  thresholdPeg                     │
                                    │  thresholdDcfMos                  │
                                    │  thresholdPeBand                  │
                                    │  thresholdPbBand                  │
                                    │  thresholdDdm                     │
                                    │  thresholdPs                      │
                                    │  createdAt / updatedAt            │
                                    └──────────────────────────────────┘

  FK rules:
  ─────────
  screenerTemplates.nlAuditId  → nlAuditLog.id         (nullable: builtin/python have no audit)
  screenerResults.templateId   → screenerTemplates.id   (nullable: ad-hoc runs)
  valuationLists.sourceResultId→ screenerResults.id     (nullable: manually built lists)
  nlAuditLog.templateId        → screenerTemplates.id   (nullable: until Accept clicked)
  nlAuditLog.resultId          → screenerResults.id     (nullable: until Run Screen done)
```

---

### 8.6 Threshold Controls — No DB Write Path

```
  User adjusts threshold slider / dropdown (Block B)
             │
             │  oninput / onchange event fires
             ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │  In-memory recalculation only                                     │
  │                                                                   │
  │  Re-evaluate Composite Score for all rows in active list:        │
  │    score = baseMoS + Σ(model passes × 10)                        │
  │                                                                   │
  │  Re-render table rows (highlight on/off)                         │
  │                                                                   │
  │  NO SQL executed — pure frontend arithmetic                       │
  └──────────────────────────────────────────────────────────────────┘
             │
             │ Only persisted when user explicitly saves the list
             ▼
  ┌──────────────────────────────────────────────────────────────────┐
  │  INSERT / UPDATE valuationLists                                   │
  │  { ..., thresholdPeg, thresholdDcfMos, thresholdPeBand, ... }   │
  │  ← threshold snapshot saved alongside symbol list                │
  └──────────────────────────────────────────────────────────────────┘
```
