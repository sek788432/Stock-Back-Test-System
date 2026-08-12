# 0016 — Introduce the selectable-condition Strategy seam

- **Status**: Accepted
- **Date**: 2026-08-12
- **Deciders**: repository maintainer
- **Supersedes**: —
- **Superseded by**: —

## Context

The starter Backtest executes one hard-coded market buy and has no user
strategy. The accepted architecture requires Selectable Conditions to be a
typed C++ plan evaluated with project-owned streaming indicators, while the
engine remains the only authority over order timing, fills, and accounting.

The first implementation needs a small executable seam that can serve the Qt
form now and later serve saved plans, the screener, and Python-equivalence
tests. Putting condition checks in the Qt tab or interpreting them directly in
the Engine would duplicate policy and make chronology difficult to verify.

## Decision

- Add `Indicators` and `Strategy` modules to the existing CMake graph.
  `Indicators` depends only on Core; `Strategy` depends on Core and
  Indicators; Engine consumes a typed `SelectableStrategyPlan`.
- A plan has separate buy and sell flat condition groups. Each group selects
  explicit `all` (AND) or `any` (OR) composition; nested groups remain future
  scope. Conditions compare a bar field, the percentage close change from the
  prior actual bar, or a named indicator output against a finite threshold.
- The Strategy module validates a plan before creating its evaluator. It owns
  indicator warm-up and returns a `buy`, `sell`, or `hold` signal for each
  actual bar. Insufficient warm-up returns `hold`; it never fabricates values
  or looks ahead.
- The Engine applies a returned signal only as an order for the next actual
  bar. It owns all order activation, adverse slippage, cash/position updates,
  and final marking. The first slice keeps the existing starter-order path for
  compatibility.
- The initial Qt form creates only this typed plan. It exposes buy/sell
  predicates and flat ALL/ANY selection; it does not save artifacts yet.

## Consequences

- Technical analysis stays reusable and independent from Qt and execution.
- The initial engine behavior is intentionally a narrow long-only vertical
  slice: a buy opens the configured quantity and a sell closes it. Shorting,
  persisted plans, nested groups, portfolios gates, and result persistence
  remain planned under Specs 05 and 07.
- The condition evaluator and the engine both need positive, invalid-input,
  warm-up/boundary, and next-bar timing tests. The Qt form must prove that it
  supplies the selected typed plan to the backend.

## Alternatives considered

1. **Interpret condition widgets in the Qt tab.** Rejected because the UI
   would own executable trading policy and the screener could not reuse it.
2. **Add condition checks directly to `runBacktest`.** Rejected because
   indicator state and plan validation would become Engine responsibilities.
3. **Generate Python for every condition plan.** Rejected because the accepted
   source of truth is a typed C++ plan, and Python is a separate planned mode.

## References

- [Architecture](../Specs/01Architecture.md)
- [Strategy Authoring](../Specs/05StrategyAuthoring.md)
- [Indicators](../Specs/06Indicators.md)
- [Engine, Replay, P&L, and Results](../Specs/07EngineReplayPnL.md)
