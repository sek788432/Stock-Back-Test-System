# Stock Screener — UI Overview Spec

**Live prototype:** [`screener_v2.html`](./screener_v2.html)  
**Parent spec:** [`11_Stock_Screener_KLine_Product.md`](../11_Stock_Screener_KLine_Product.md)  
**Full index:** [`README.md`](./README.md)  
**Version:** v11.0

---

## Frontend Screenshot

![Stock Screener Frontend](./frontend.PNG)

---

## Feature Overview

The Stock Screener & Valuation platform lets a user narrow a stock universe down to a ranked candidate list (Block A), then score those candidates across multiple valuation models side by side (Block B).

### Two Blocks

**Block A — Screener**

The user defines screening conditions in one of three modes:

- **Built-in (form-driven):** Drag-and-drop condition cards (e.g. ROE ≥ 15%, EPS trend stable). Conditions are combined with a single **AND** (all must pass) or **OR** (any one passes) selector.
- **Python Script:** User writes a `screen(symbol, bars, as_of) -> bool | float` function directly. The system runs it in a hardened sandbox per symbol (500 ms timeout, 128 MB limit, no I/O or network).
- **Natural Language (AI):** User describes criteria in plain text; Claude generates the Python equivalent. The user must explicitly click **Accept** before the code is allowed to run — silent auto-execution is never permitted.

The output is a ranked symbol list (rank, name, price, change %, market cap, sector) that can be saved to the database and fed into Block B.

**Block B — Valuation Matrix**

Eight valuation models are computed in parallel for a set of symbols. Each model either highlights (green) or does not. A Composite Score (0–120 pts) aggregates the results:

| Model | Signal |
|---|---|
| **PEG Ratio** | `pe / (epsGrowth5Y × 100) < threshold` |
| **DCF + MoS** | Two-stage discounted cash flow; Margin of Safety ≥ threshold (base score driver: up to 50 pts) |
| **P/E Band** | Current P/E vs. sector average — which quartile band |
| **P/B Band** | Same structure, using P/B |
| **DDM Yield** | Dividend yield ≥ threshold |
| **P/S Ratio** | P/S vs. sector median |
| **P/E vs. Historical Avg** | Current P/E below own 5-year average (auto-comparison, no user threshold) |
| **Yield vs. Historical Avg** | Yield above own 5-year average — price fell relative to dividend (direction inverted vs. P/E) |

A **Technical Signal** (moving-average alignment + volume ratio) is displayed separately and does not affect the Score.

### Two Databases

| Database | Writer | Reader | Contents |
|---|---|---|---|
| `MarketData.duckdb` | Python pipeline | C++ (read-only) | `hourlyBars`, `fundamentals`, `stocks`, `indexConstituents` (Phase 2) |
| `app.db` (SQLite) | C++ app | C++ app | `screenerTemplates`, `screenerResults`, `valuationLists`, `nlAuditLog` |

C++ **never writes to DuckDB** — this is a hard constraint inherited from `04_Data_Layer.md`. Market data ownership stays with the Python pipeline; application state ownership stays with C++.

### NL Audit Trail

Every AI generation turn is persisted immediately (before the user decides):

```
Send prompt → INSERT nlAuditLog (accepted=0)
Accept      → UPDATE accepted=1 · INSERT screenerTemplates · link templateId back
Run Screen  → INSERT screenerResults · link resultId back
```

Rejected turns are **never deleted** — `accepted=0, acceptedAt=NULL` is the permanent record of a rejection. The full chain (`nlAuditLog ↔ screenerTemplates ↔ screenerResults`) satisfies the litigation-grade traceability requirement in Spec 11 §3.2.

### No-Lookahead Guarantee

Every DB query that touches `fundamentals` or `hourlyBars` includes `WHERE asOfDate <= asOfEnd` (or equivalent). No future data can leak into a historical screen run.

---

## Spec Map

This document covers **UI behaviour only**. For logic and implementation details, use the spec that matches your question:

| I want to know... | Read |
|---|---|
| Every control, table column, and UI state | This document (below) |
| C++ classes for condition blocks and AND/OR evaluation | [`Spec_A — Screener Engine`](./Spec_A_Screener_Engine.md) |
| Valuation model formulas (PEG, DCF, P/E Band…) and Composite Score | [`Spec_B — Valuation Engine`](./Spec_B_Valuation_Engine.md) |
| Database schema — which tables exist, which columns, who writes | [`Spec_C — Database`](./Spec_C_Database.md) |
| Python `screen()` API, sandbox rules, NL/AI bridge | [`Spec_D — NL / Python Runtime`](./Spec_D_NL_Python_Runtime.md) |

---

## Layout Overview

The screen is divided into two independent vertical blocks:

```
+----------------------------------------------------------+
|  HEADER  — Platform name + data date                     |
+----------------------------------------------------------+
|                                                          |
|  BLOCK A — Screener                                      |
|  +-- Universe Controls Row --------------------------+   |
|  | Stock Universe | As-of Date Range | Cadence | Import| |
|  +----------------------------------------------------+   |
|  +-- Mode Tabs ----------------------------------------+  |
|  | [Built-in]  [Python Script]  [Natural Language]    |  |
|  +----------------------------------------------------+   |
|  +-- Mode Panel (switches per tab) -------------------+   |
|  |  ...                                               |   |
|  +----------------------------------------------------+   |
|  +-- Run & Results Bar --------------------------------+  |
|  | Status | Export CSV | Copy | [Run Screen]           |  |
|  +----------------------------------------------------+   |
|  +-- Results Table (appears after Run) ---------------+   |
|  | Rank | Symbol | Name | Price | Chg% | Cap | Sector |  |
|  +----------------------------------------------------+   |
|                                                          |
+----------------------------------------------------------+
|                                                          |
|  BLOCK B — Valuation                                     |
|  +-- Source Row --------------------------------------+   |
|  | Source A (DB List) | Source B (manual) | Source C  |  |
|  +----------------------------------------------------+   |
|  +-- Threshold Controls --------------------------------+  |
|  | PEG | DCF MoS | P/E Band | P/B Band | DDM | P/S    |  |
|  +----------------------------------------------------+   |
|  +-- Valuation Table ----------------------------------+   |
|  | Ticker | Score | 6 model cols | Technical Signal    |  |
|  +----------------------------------------------------+   |
|                                                          |
+----------------------------------------------------------+
```

---

## BLOCK A — Screener

![Block A — Screener (Built-in Conditions mode)](./frontend%20-%20block%20A.PNG)

### A1 · Universe Controls Row

Four controls in a single row defining *what to screen* and *at what point in time*.

| Control | Type | Description |
|---|---|---|
| **Stock Universe** | `<select>` | Defines the symbol pool: S&P 500 / NASDAQ 100 / NYSE All Listed / Custom Watchlist. |
| **As-of Date Range** | Two `<input type="date">` (Start → End) | Evaluation window. **Strict no-lookahead**: only bars with date ≤ End date are used. See Spec §2.3. |
| **Refresh Cadence** | `<select>` | On Demand (manual) / Daily (after close) / Weekly. Scheduled runs are handled by the Launcher layer (Spec §09), not the engine core. |
| **Import Custom Universe** | Dashed button | Import a custom symbol list from XLSX / CSV, overriding the Universe dropdown. |

> **Design principle:** Universe controls *what* to run; As-of Date controls *when* to evaluate. Both are set independently.

---

### A2 · Mode Selector Tabs

Three tabs that replace the entire panel content, corresponding to the three authoring modes in Spec §2.1 / §3.

```
[ Built-in Conditions ]  [ Python Script ]  [ Natural Language (AI) ]
  ^ active = blue bg       ^ inactive = dim   ^ inactive = dim
```

The Universe Controls Row and Run Bar remain visible regardless of the active tab.

---

### A3 · Panel: Built-in Conditions (Mode 1)

The default mode shown in the screenshot.

#### A3-a · Templates Row

```
Templates: [ Buffett Value ]  [ Momentum Breakout ]  [user-saved templates...]
                                                  Logic: [ ALL (AND) ]  ANY (OR)
```

| Element | Description |
|---|---|
| **Buffett Value** | Loads: EPS stable + ROE >= 15% + Gross Margin >= 30% + D/E <= 50% + FCF continuously positive |
| **Momentum Breakout** | Loads: 60D high breakout + Volume ratio >= 2.5X + Bullish MA alignment |
| **User-saved templates** | Appear dynamically (purple) after saving; click to reload |
| **ALL (AND)** | Green active state — all conditions must pass for inclusion |
| **ANY (OR)** | Amber active state — any single condition passing is sufficient |

> Toggling logic instantly updates the label text and color below (AND = green, OR = amber).

#### A3-b · Active Criteria Shelf

```
+-- Stability   EPS_5Y_Trend              Drop<=1Y  x --+
|   Moat        ROE_3Y_Avg                >= 15%    x   |
|   Moat        Gross_Margin_3Y_Avg       >= 30%    x   |  <- scrolls when full
|   Leverage    Debt_to_Equity            <= 50%    x   |
|   Liquidity   FCF_5Y_Status   Continuous_Pos      x   |
|   Fundamental Dividend_Yield            >= 4.0%   x   |
+-------------------------------------------------------+
```

Each block contains:
- **Category badge** (left): classification tag — Fundamental = green, Technical = amber
- **Field name** (center): indicator key in monospace
- **Value** (right): threshold or condition value, blue bold
- **x button**: removes the block immediately

#### A3-c · Add Factor / Save Template

```
Add Factor:  [ + Fundamental ]  [ + Technical ]  [ Save as Template ]
```

| Button | Behavior |
|---|---|
| **+ Fundamental** | Inserts a default `Dividend_Yield >= 4.0%` block |
| **+ Technical** | Inserts a default `Price > 200D_MA` block |
| **Save as Template** | Prompts for a name → saves current condition set → appears in Templates row |

---

### A4 · Panel: Python Script (Mode 2)

Corresponds to Spec §3 Mode 2. Allows writing a Python predicate directly.

```
+-- Info bar --------------------------------------------------------+
| Script receives symbol: str, bars: DataFrame, as_of: datetime      |
|    Return True/float to include, False to exclude.                  |
|    No I/O, network, or filesystem access (sandboxed).              |
+--------------------------------------------------------------------+
+-- Line # --+-- Code Editor -----------------------------------------+
|  1         | def screen(symbol: str, bars, as_of) -> bool|float:   |
|  2         |     """..."""                                           |
|  3         |     roe = bars['roe'].iloc[-1]                         |
|  ...       |     ...                                                |
+------------+-------------------------------------------------------+
+-- Lint Panel (shown after Validate) --------------------------------+
| OK   No syntax errors detected                                      |
| W001 Line 2: docstring could include param types                    |
| OK   All return paths return bool or float                          |
+--------------------------------------------------------------------+
[ Validate ]  [ Compile ]   status: Lint passed (1 warning)
```

| Element | Description |
|---|---|
| **Line number gutter** | Auto-updates line count; scroll is synced with the editor |
| **Code editor** | Monospace font, tab = 4 spaces, spellcheck off |
| **Lint panel** | Shows OK (green), W xxx (amber), E xxx (red); hidden until Validate is clicked |
| **Validate** | Runs static analysis and populates the lint panel |
| **Compile** | Compiles the script into an executable adaptor; must succeed before Run Screen |

> **Sandbox rules (Spec §3.1):** I/O, network, and filesystem access are prohibited — same sandbox posture as Python strategies.

---

### A5 · Panel: Natural Language (AI) (Mode 3)

Corresponds to Spec §3 Mode 3. Chat-style interaction with mandatory Accept before execution.

```
+-- Safety Warning ---------------------------------------------------+
| AI-generated conditions require explicit user acceptance             |
|   before execution. Prompt + model + hash stored for audit.         |
+---------------------------------------------------------------------+
+-- Chat History ------------------------------------------------------+
|                                                                      |
|   [Conversation starts here...]                                      |
|                                                                      |
|                    +-- User message (right, purple bubble) ---+      |
|                    | Find stocks with ROE > 15%...            |      |
|                    +------------------------------------------+      |
|   +-- AI Draft  ·  claude-sonnet-4-6 · a3f92e1c ----------+         |
|   | def screen(symbol,...):                                 |         |
|   |     ...                                                 |         |
|   | [ Accept & Compile ]  [ Reject ]                       |         |
|   +---------------------------------------------------------+         |
|                                                                      |
+---------------------------------------------------------------------+
+-- Input area --------------------------------------------------------+
|  [ Describe your criteria or refine the last suggestion...  ]  Send |
|                                              Shift+Enter = new line  |
+---------------------------------------------------------------------+
[ Condition accepted & compiled. Audit record saved. ]  <- appears on Accept
```

| Element | Description |
|---|---|
| **Safety warning** | Always visible, cannot be dismissed |
| **Chat history** | Scrollable container preserving full conversation; user bubble (right/purple), AI bubble (left/dark) |
| **AI bubble header** | Shows model name + source hash (for audit) |
| **Generated code** | Displayed as `<pre>`, green, read-only, horizontally scrollable |
| **Accept & Compile** | Marks this bubble as accepted (green border), supersedes other AI turns, shows global confirmation |
| **Reject** | Marks turn as rejected; input box regains focus for refinement |
| **Input area** | `Shift+Enter` for new line, `Enter` to send; supports multi-turn refinement |
| **Audit record** | On Accept: stores originating prompt + model ID + source hash (Spec §3.2) |

> **Accept is required before Run Screen.** Silent execution is not permitted (Spec §3.1).

---

### A6 · Run & Results Bar

Pinned to the bottom of Block A. Remains visible across all tab switches.

```
Status: Complete  ·  5 matches           [ Export CSV ]  [ Copy ]  [ Run Screen ]
```

**Run Screen flow:**
1. Click → Status becomes "Running…" (amber)
2. Complete → Status becomes "Complete" (green) + match count displayed
3. Results Table, Export CSV, Copy, and Save to Database all appear

#### Results Table (Spec §2.4 minimum columns)

| Column | Description |
|---|---|
| **Rank** | Sequential rank starting at 1 |
| **Symbol** | Ticker in monospace bold |
| **Company Name** | Full company name (when available) |
| **Last Price** | Most recent closing price in `$xxx.xx` format |
| **Change %** | Day change — positive = green, negative = red |
| **Market Cap** | Market capitalisation (T / B suffix) |
| **Sector** | Industry classification from the data pipeline |
| **+ Add** | Pushes this symbol directly into Block B |

```
[ Save List to Database ]  <- saves results; appears as an option in Block B Source A
```

---

## BLOCK B — Valuation

![Block B — Valuation Matrix (composite scoring)](./frontend%20-%20blockB.PNG)

Independent of the Screener. Runs cross-model valuation scoring across a set of symbols.

### B1 · Source Row (three input paths)

```
Source A — Load from Database   Source B — Append Ticker       Source C — Direct Import
[ Default: Core Tech (3) v ]    [ NVDA _________ ]  [ Append ] [ Excel/CSV ]
```

| Source | Description |
|---|---|
| **Source A** | Loads a saved screener list from the database; dropdown selects which list |
| **Source B** | Manually type a ticker (uppercased), click Append to add |
| **Source C** | Import an external spreadsheet (Excel / CSV) |

All three sources can be **combined simultaneously**. The active list is the deduplicated union of all three.

---

### B2 · Valuation Highlight Thresholds

Six threshold controls that instantly affect the **highlight styling** of the table below (coloring only — not filtering).

| # | Name | Control | Logic |
|---|---|---|---|
| 1 | **PEG Target** | Number input (default 1.0) | PEG Ratio < threshold → highlight |
| 2 | **DCF Margin of Safety** | Number input + % | MoS >= threshold → highlight |
| 3 | **P/E Band** | Dropdown (Lower-Mid / Bottom Track) | pe_band matches → highlight |
| 4 | **P/B Band** | Dropdown (Bottom Track / Standard) | pb_band matches → highlight |
| 5 | **DDM Yield** | Number input + % | Dividend Yield >= threshold → highlight |
| 6 | **P/S Industry Level** | Dropdown (Undervalued / Median) | ps_status matches → highlight |

> Any control change triggers an immediate table redraw — no confirm button needed.

---

### B3 · Valuation Table

Core output table. Each row = one symbol, columns compare six valuation models side by side.

```
[icon]  Ticker  Score  PEG  DCF(MoS)  P/E Band  P/B Band  DDM  P/S  Technical  [x]
```

| Column | Description |
|---|---|
| **Ticker** | Ticker symbol, white bold monospace |
| **Score** | 0–120 pts in blue. Reflects overall valuation attractiveness (see formula below) |
| **1. PEG Model** | PEG Ratio value; below threshold → green highlight |
| **2. DCF (MoS)** | DCF target price + margin of safety %; MoS >= threshold → green highlight |
| **3. P/E Band** | P/E ratio band classification; matches threshold → green highlight |
| **4. P/B Band** | P/B ratio band classification; matches threshold → green highlight |
| **5. DDM Yield** | Dividend yield; >= threshold → green highlight |
| **6. P/S Ratio** | Price-to-sales ratio + relative industry position; matches threshold → green highlight |
| **7. P/E vs Avg** | Current P/E shown alongside 5Y and 10Y historical averages. `pe < peAvg5Y` → green highlight (stock cheaper than its own history). **No user threshold** — auto-comparison. |
| **8. Yield vs Avg** | Current yield shown alongside 5Y and 10Y historical averages. `yield > yieldAvg5Y` → green highlight (yield above history = price fell = potentially cheaper). **Direction inverted vs P/E.** |
| **Technical Timing Signal** | 🟢/🟡/🔴 + short description. **Decoupled from valuation** (amber background). Timing reference only — does not affect Score. |
| **x (delete)** | Removes the row from the active list instantly |

#### Score Formula

```
Base score (DCF Margin of Safety):
  MoS >= 30%  ->  50 pts
  MoS >= 15%  ->  35 pts
  MoS <  15%  ->  10 pts

Per-model bonus (each model meeting its threshold adds +10 pts):
  PEG < threshold       -> +10
  P/E band match        -> +10
  P/B band match        -> +10
  DDM yield >= target   -> +10
  P/S status match      -> +10
  P/E < 5Y avg          -> +10   (cheaper than own history)
  Yield > 5Y avg        -> +10   (yield above history = price fell)

Cap: 120 pts
```

> **Note on columns 7 & 8:** These use the stock's own historical average as a benchmark,  
> not a user-set threshold. The highlight fires automatically — there is no control in the Threshold panel for these two.

#### Technical Signal Color Semantics

| Color | Meaning |
|---|---|
| 🟢 | Bullish signal (breakout, support held, uptrend) |
| 🟡 | Neutral / wait (high volatility, consolidation, sideways) |
| 🔴 | Bearish signal (MA death cross, downtrend) |

---

## Data Flow

```
Templates (Buffett / Momentum)
         |
         v load
Universe + As-of Date Range --> Condition Shelf (AND / OR)
                                       |
                                       v
                              Screener Results Table
                         (Rank / Symbol / Name / Price / Chg% / Cap / Sector)
                                       |
                                       v
                              [ Save to Database ]
                                       |
              +------------------------+
              |                        |
         Source A (DB)          Source B / C (manual / import)
              |                        |
              +----------+-------------+
                         |
                         v
                   Active Valuation List
                         |
                         v
              Threshold Controls (6 items)
                         |
                         v
                   Valuation Table
            (Score + 6 model columns + Technical)
```

---

## Screener State Machine

```
Initial state
  |
  +-- Switch tab        -> Mode panel replaced (Universe row unchanged)
  +-- Load template     -> Condition blocks populate the shelf
  +-- Toggle AND / OR   -> Label color updates instantly
  |
  v  Click Run Screen
Running... (Status = amber)
  |
  v  Complete
Complete (Status = green)
  +-- Results Table appears
  +-- Export CSV / Copy appear
  +-- Save to Database appears
            |
            v  Save
New entry in DB -> Block B Source A dropdown gains a new option
```

---

## Spec 11 Reference Index

| Section | Spec 11 Clause |
|---|---|
| A1 Universe / As-of / Cadence | §2.3 Universe & run metadata |
| A3 Built-in Conditions + AND/OR | §2.1 Mode 1, §2.2 Logical composition |
| A4 Python Script + Lint | §3 Mode 2, §3.1 Sandbox |
| A5 NL Chat + Accept / Audit | §3 Mode 3, §3.1 Safety, §3.2 Traceability |
| A6 Results Table columns | §2.4 Results presentation |
| A6 Export CSV / clipboard | §2.4 Exports |
| B1–B3 Valuation | Extended feature — not defined in Spec 11 |
