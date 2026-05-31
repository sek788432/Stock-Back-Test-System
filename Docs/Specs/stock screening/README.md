# Stock Screener & Valuation — Sub-Spec Index

> **Start here.** This folder contains all design documents for the Stock Screener (Block A) and Valuation Matrix (Block B) features defined in [`11_Stock_Screener_KLine_Product.md`](../11_Stock_Screener_KLine_Product.md).

---

## Live Frontend Prototype

Open [`screener_v2.html`](./screener_v2.html) in any browser to see the interactive prototype.  
No build step required — it runs entirely in the browser.

---

## What It Looks Like

![Stock Screener & Valuation Platform](./frontend.PNG)

The UI has two blocks:

| Block | What it does |
|---|---|
| **Block A — Screener** | Filter a stock universe by conditions (built-in / Python / AI). Output: ranked symbol list. |
| **Block B — Valuation** | Score each symbol across 8 valuation models. Output: composite score + highlights. |

---

## Read Order — Start Here

If you are new to this feature, read in this order:

```
1. This README          ← you are here
2. Screener_UI_Overview.md   ← full UI walkthrough with screenshots and ASCII diagrams
3. Spec_C_Database.md        ← data model (read this before A/B/D — they all reference it)
4. Spec_A_Screener_Engine.md ← Block A logic: C++ classes, AND/OR evaluation, DB queries
5. Spec_B_Valuation_Engine.md← Block B logic: 8 model formulas, composite score, C++ headers
6. Spec_D_NL_Python_Runtime.md ← Python sandbox + AI bridge for Mode 2 and Mode 3
```

---

## File Index

### Documentation

| File | What it covers |
|---|---|
| [`README.md`](./README.md) | This file — entry point and navigation guide |
| [`Screener_UI_Overview.md`](./Screener_UI_Overview.md) | Complete UI spec: every control, every table column, data flow diagrams, state machine. **Includes embedded screenshot.** |
| [`Spec_A_Screener_Engine.md`](./Spec_A_Screener_Engine.md) | Block A engine: `ConditionBlock` C++ headers, AND/OR evaluation algorithm, `IFundamentalsRepository`, DuckDB SQL patterns, `AppDb` SQLite operations |
| [`Spec_B_Valuation_Engine.md`](./Spec_B_Valuation_Engine.md) | Block B engine: all 8 model formulas (PEG, DCF, P/E Band, P/B Band, DDM, P/S, P/E vs Avg, Yield vs Avg), composite score (0–120 pts), technical signal logic |
| [`Spec_C_Database.md`](./Spec_C_Database.md) | DB schema: `MarketData.duckdb` (hourlyBars, fundamentals, stocks) + `app.db` SQLite (screenerTemplates, screenerResults, valuationLists, nlAuditLog). Includes 6 data flow diagrams and 7 user workflow SQL examples. |
| [`Spec_D_NL_Python_Runtime.md`](./Spec_D_NL_Python_Runtime.md) | Python `screen()` API contract (all 21 `bars` columns), sandbox rules (forbidden imports, timeouts, memory limits), NL/AI bridge (Claude API), audit write flow |
| [`API_Data_Requirements.md`](./API_Data_Requirements.md) | **What to fetch and compute** — every field in `fundamentals` mapped to its raw API source, computation formula, and minimum historical depth. Reference for the Python pipeline. |

### Frontend

| File | What it is |
|---|---|
| [`screener_v2.html`](./screener_v2.html) | **Current prototype** — open in browser. Fully interactive (built-in conditions, Python editor with line numbers + lint, NL chat UI, valuation matrix). |
| [`screener.html`](./screener.html) | Original prototype (archived — superseded by v2) |

### Assets

| File | What it is |
|---|---|
| [`frontend.PNG`](./frontend.PNG) | Full-page screenshot of `screener_v2.html` — used in `Screener_UI_Overview.md` and this README |
| [`frontend - block A.PNG`](./frontend%20-%20block%20A.PNG) | Block A (Screener) highlighted — used in `Screener_UI_Overview.md §BLOCK A` |
| [`frontend - blockB.PNG`](./frontend%20-%20blockB.PNG) | Block B (Valuation Matrix) highlighted — used in `Screener_UI_Overview.md §BLOCK B` |
| [`frontend - python script.PNG`](./frontend%20-%20python%20script.PNG) | Python Script mode UI — used in `Spec_D §10 Mode 2` |
| [`frontend - NL AI.PNG`](./frontend%20-%20NL%20AI.PNG) | Natural Language (AI) mode UI — used in `Spec_D §10 Mode 3` |

---

## Spec Map — Which Spec Answers Which Question

| Question | Go to |
|---|---|
| What external API fields does the pipeline need to fetch? | [`API_Data_Requirements.md`](./API_Data_Requirements.md) |
| Which fields must be computed vs fetched directly? | [`API_Data_Requirements.md §2`](./API_Data_Requirements.md) |
| How much history does the pipeline need to seed? | [`API_Data_Requirements.md §3`](./API_Data_Requirements.md) |
| What does the UI look like? What does each button do? | [`Screener_UI_Overview.md`](./Screener_UI_Overview.md) |
| What is the JSON format for a condition block? | [`Spec_A §4`](./Spec_A_Screener_Engine.md) |
| How does AND / OR evaluation work in C++? | [`Spec_A §9`](./Spec_A_Screener_Engine.md) |
| Which DB tables exist and what are their columns? | [`Spec_C §3–4`](./Spec_C_Database.md) |
| How does the app save a screener template? | [`Spec_C §6 Example 1`](./Spec_C_Database.md), [`Spec_A §10`](./Spec_A_Screener_Engine.md) |
| How is the PEG / DCF / P/E Band model calculated? | [`Spec_B §7`](./Spec_B_Valuation_Engine.md) |
| How is the Composite Score (0–120 pts) calculated? | [`Spec_B §8`](./Spec_B_Valuation_Engine.md) |
| What columns does a Python `screen()` script receive? | [`Spec_D §3.2`](./Spec_D_NL_Python_Runtime.md) |
| What imports are forbidden in a Python script? | [`Spec_D §6`](./Spec_D_NL_Python_Runtime.md) |
| How does the NL/AI mode generate and audit code? | [`Spec_D §7–9`](./Spec_D_NL_Python_Runtime.md), [`Spec_C §6 Example 7`](./Spec_C_Database.md) |
| Why are there two databases? Who writes each? | [`Spec_C §2`](./Spec_C_Database.md) |
| Where is the P/E vs Avg / Yield vs Avg logic? | [`Spec_B §7 Models 7–8`](./Spec_B_Valuation_Engine.md) |
| How does the Qt UI call the screener engine? | [`Spec_A §12`](./Spec_A_Screener_Engine.md) |
| How does the Qt UI call the valuation engine? | [`Spec_B §10`](./Spec_B_Valuation_Engine.md) |

---

## Key Design Decisions (Quick Reference)

| Decision | Where it's defined |
|---|---|
| DuckDB is **read-only** from C++ | [`04_Data_Layer.md`](../04_Data_Layer.md) — project-wide rule |
| App data uses **SQLite** (`app.db`), not DuckDB | [`Spec_C §2`](./Spec_C_Database.md) — C++ must write; DuckDB is Python's |
| Python execution uses **interface** (`ISandboxRunner`) — impl TBD via ADR | [`Spec_D §5`](./Spec_D_NL_Python_Runtime.md) |
| NL-generated code requires **explicit user Accept** before running | [`Spec_D §9`](./Spec_D_NL_Python_Runtime.md), [Spec 11 §3.1](../11_Stock_Screener_KLine_Product.md) |
| Composite Score max is **120 pts** (8 models × 10 + 50 base) | [`Spec_B §8`](./Spec_B_Valuation_Engine.md) |
| Technical Signal is **decoupled** from score (display only) | [`Spec_B §7`](./Spec_B_Valuation_Engine.md) |
| Yield vs Avg direction is **inverted** vs P/E vs Avg | [`Spec_B §7 Model 8`](./Spec_B_Valuation_Engine.md) |

---

## Parent Specs (outside this folder)

| Spec | Why it matters here |
|---|---|
| [`11_Stock_Screener_KLine_Product.md`](../11_Stock_Screener_KLine_Product.md) | Product contract — defines the three modes, AND/OR requirement, result columns (§2.4), NL safety rules (§3.1), audit traceability (§3.2) |
| [`04_Data_Layer.md`](../04_Data_Layer.md) | DuckDB read-only rule, `BarStream` API, `DataSource` connection patterns |
| [`03_Backend_Core.md`](../03_Backend_Core.md) | Naming conventions, `Result<T>`, `Error`, `Timestamp` — used throughout A/B/D |
| [`02_Frontend_Qt.md`](../02_Frontend_Qt.md) | Threading rules (never block UI thread), widget conventions |
