# 11 — K-Line Replay, Strategy Modes, & Stock Screener (Product Requirements)

This spec captures **user-visible capabilities** aligned with the application blueprint: **K-line (candlestick) replay**, **strategy authoring in three modes**, and **universe stock selection / screening** using the same three modes. It refines `00`, `02`, `05`, and `07` without replacing their engineering detail.

When this document disagrees with a deeper spec on **implementation** (for example, which embeddable language ships first), treat this file as the **product contract**; resolve gaps with an ADR and then update the implementation spec.

---

## 1. K-line replay backtest

### 1.1 Definition

**K-line replay** is a **single-symbol**, **time-synchronized** playback of historical OHLCV bars on a candlestick chart, while the **same strategy and broker model** as batch backtest drive orders, fills, portfolio state, and P&L **one bar at a time** under user-controlled pacing (`07` §5).

### 1.2 Required inputs (setup)

| Input | Requirement |
| --- | --- |
| **Symbol** | User-selected tradable symbol present in the data layer (`04`). |
| **Timeframe** | Maps to a **schema** / bar resolution in DuckDB (e.g. daily, hourly); must match existing `schemaName` discovery rules. |
| **Start / end** | Inclusive UTC date-time or date-only range constrained to available history for that symbol + schema. |
| **Initial capital** | Positive cash balance at replay start (`07` §1 `initialCash`). |
| Built-in components | Whatever strategy bundle is active in the Strategy editor (`05`) for this session (built-in rule set, Python, or artifact produced from natural language — §3). |

### 1.3 Required UI behaviors

- **Chart**: Candlesticks + volume pane; visible window clipped to performance limits in `02` §4 (`IChartView`).
- **Playback**: Play/pause, step forward/back where supported (`07` §5 scrub), speed multiplier (including “max”).
- **Markers**: Distinct buy/sell markers at fill timestamps/prices (`02` §2.3).
- **Portfolio strip**: Cash, position size / market value, total equity, realized and unrealized P&L (`07` §3).
- **Trade log**: Tabular audit trail compatible with batch backtest fields (timestamp, side, qty, price, fees, cash after, optional P&L attribution) (`07` §4).

### 1.4 Consistency with batch backtest

For the **same** `(symbol, schema, date range, engine config, strategy definition, data snapshot)`, **replay stepping through all bars** must reach the **same** final trades and equity series as **`run(...)` without pacing** modulo explicit timing differences solely from replay clock sleeps (`07` §8 determinism applies to functional outputs, not wall-clock pacing).

---

## 2. Stock selection / screener (universe filtering)

Stock selection identifies a **subset of symbols** (a **universe**) that satisfy declarative criteria at a **reference date** (or rolling “as-of” cadence). It shares the **condition model** philosophy with strategy rules but **outputs a ranked/filtered symbol list**, not orders.

### 2.1 Modes (must match §3 structurally)

1. **Built-in conditions** — form-driven indicators, thresholds, crosses, fundamentals-style fields once available, combined with explicit **logical composition** (§2.2).
2. **Python script** — user-authored script expressing per-symbol predicates and optional ranking; runs in the same trust/sandbox posture as Python strategies (`05` §5).
3. **Natural language (AI)** — user prompt is transformed by an **assistant** into reviewable Python or structured conditions; user confirms before execution (§3.3).

### 2.2 Logical composition for built-in conditions

Built-in screening rows SHALL support:

| Composition | Meaning |
| --- | --- |
| **ALL (AND)** | Every configured condition must evaluate true for inclusion. |
| **ANY (OR)** | At least one condition must evaluate true. |

**Stretch (document for future refinement, not MVP gate):** nested groups with `(A AND B) OR (C AND D)` via a bracketed AST or explicit group IDs — until then, MVP is flat AND or flat OR selector plus ordering of rows for readability only.

### 2.3 Universe & run metadata

| Field | Notes |
| --- | --- |
| **Universe** | Enumerated set: e.g. US listed (NASDAQ + NYSE), index constituents, watchlist — exact sources live in data pipeline / config (`04`). |
| **As-of date / range** | Evaluation uses bars **≤ chosen timestamp** only — **no lookahead** across the evaluated bar. |
| **Refresh cadence** | Batch on demand vs scheduled (e.g. daily after close); scheduled runs are Launcher/app-level jobs (`09`), not engine core. |

### 2.4 Results presentation

Minimum columns: **rank**, **symbol**, **human-readable company name** (when available), **last price**, **change %**, **market cap**, **sector** (or closest available classification from data pipeline). Tabs or views for simple **performance attribution** vs benchmark and **sector breakdown** may follow in later phases (`02` will host layout).

Exports (CSV / clipboard) SHOULD reuse the same table model types as metrics exports for consistency.

### 2.5 Engine coupling

Phase 1 may implement screening as:

- sequential per-symbol indicator evaluation backed by existing `Indicators`/`BarStream` APIs; or
- batched prefetch where `04` exposes efficient multi-symbol windows.

Heavy cross-sectional workloads remain subject to **`10`** performance hygiene (no needless all-history loads).

---

## 3. Strategy input & natural language bridging

Three **first-class authoring modes** SHALL appear in UI and persistence:

| Mode | User experience | Compiled / loaded artifact |
| --- | --- | --- |
| **1. Built-in components (no code)** | Form rows: type (indicator, filter, portfolio gate, …), component id, parameters; optional AND/OR only where the rule schema supports Boolean trees — see `05` §3. | `*.rule.json` (or successor schema) compiling to `IStrategy`. |
| **2. Python script** | `QPlainTextEdit` (+ highlighting, lint), explicit **Validate**/**Compile** (`02` §2.1). Same persistence as other strategies (`02` §5). Compile entry point **`05` §5**. | Python-hosted strategy adaptor implementing `IStrategy` (**implementation via ADR** — embed CPython/pybind vs subprocess IPC vs translator). |
| **3. Natural language (AI agent → script)** | Prompt box + chat-style refinement; emits **candidate** Python or structured rules; requires **explicit user acceptance** before compile/run (no silent auto-trade). Audit trail stores prompt + emitted source hash/version. | Same as modes 1 or 2 after acceptance.

### 3.1 Safety & determinism posture

- NL and Python paths MUST inherit **sandboxing** analogous to Lua (`05` §4.2 sandbox shape, **`05` §5.3** Python constraints, **`05` §9** randomness/determinism; engine invariants in **`07` §8**).
- Prompts MUST NOT bypass user confirmation before running backtests attaching real capital configurations in paper/live extensions (out of scope today but design must not forbid future guardrails).

### 3.2 Traceability

Store alongside strategy files:

| Metadata | Purpose |
| --- | --- |
| Originating prompt (NL mode) | Reproducibility and user support. |
| Model / agent identifier + version string | Debugging semantic drift (`10` telemetry policies TBD). |
| Accepted/generated source snapshot | Litigation-grade replay of what actually ran.

---

## 4. Traceability matrix

| Capability | Primary specs |
| --- | --- |
| Charting & tabs layout | `02_Frontend_Qt.md` |
| Rule JSON & operators | `05_Strategy_Authoring.md` §3 |
| Python & NL authoring | `05_Strategy_Authoring.md` §5–6 |
| Engine, replay clock, P&L | `07_Engine_Replay_PnL.md` |
| Data access, no writer from C++ | `04_Data_Layer.md`, `AGENTS.md` H2 |
| Determinism CI | `07` §8, `10_CI_Dev_Flow.md` |

---

## 5. Out of scope (explicit)

- Live brokerage execution.
- Cloud sync of prompts/strategies unless added later with ADR.
- Guarantees about third-party LLM availability or pricing — NL mode is **optional subsystem** that degrades gracefully when disabled.
