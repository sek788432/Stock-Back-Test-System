# 0013 — Introduce the starter Backtest seam

- **Status**: Accepted
- **Date**: 2026-08-10
- **Deciders**: repository maintainer
- **Supersedes**: —
- **Superseded by**: —

## Context

ADR 0011 establishes the project-owned C++ engine as the authority for
chronology, execution, and accounting. The repository previously had only a
bar-only Replay implementation, while the complete accepted engine contract
also requires strategy interfaces, synchronized multi-symbol slices, general
orders, portfolio and margin behavior, metrics, and durable results.

Implementing that entire contract as one horizontal change would delay a usable
feedback loop and couple the first Backtest page to interfaces that have not yet
been exercised. Putting starter execution logic in Qt or Bindings would instead
give presentation code engine authority and violate the module graph.

## Decision

Introduce one deliberately limited vertical slice behind a narrow Engine seam:

- `runBacktest(BacktestRequest)` accepts ordered single-symbol bars, initial
  capital in microdollars, and a fixed positive whole-share quantity.
- The starter behavior submits one market buy on the first bar. It becomes
  eligible only at the next actual bar open, receives the accepted default
  1-basis-point adverse slippage, fills completely when affordable, and is
  otherwise rejected without changing cash.
- A one-bar input cancels the pending order because no future market data exists.
  A filled position remains open and is marked at the final actual close.
- Engine accounting records use scaled integers. The existing floating-point
  `Bar` is normalized at the Engine seam until the planned Core fixed-point Bar
  migration lands.
- Bindings translates the Engine result into presentation values. Frontend
  depends on Bindings and does not include Engine interfaces.
- The page owns its asynchronous run handle, propagates a cooperative
  cancellation token through Bindings, Data, and Engine, and waits for cleanup
  before the page is destroyed.
- The dedicated Backtest page names this behavior **Starter market buy** and
  states its limitation. It must not present the slice as the complete Strategy,
  broker, portfolio, metric, or `.bteresult` workflow.

Do not introduce a speculative `IStrategy` or general broker interface for this
single behavior. Add those seams with the vertical slice that provides their
second real caller or adapter and their complete contract tests.

## Consequences

- Users can execute and inspect the first real Engine path from its own page.
- Engine chronology and accounting remain localized behind one function-shaped
  interface, with positive, negative, and boundary tests at that seam.
- Frontend and Bindings stay presentation adapters rather than a second engine.
- The starter result is not a canonical Backtest Result: it has no strategy
  artifact identity, result hash, metrics, or durable `.bteresult` persistence.
- Later slices must replace or deepen the starter request/result as the accepted
  fixed-point Core, Strategy, portfolio, and Results interfaces become real.
  Existing starter behavior remains regression-tested during that migration.

## Alternatives considered

1. **Implement the complete accepted engine before adding a page.** Rejected
   because it creates a large horizontal change with delayed user feedback and
   many unexercised interfaces.
2. **Execute the starter trade in the view model.** Rejected because Bindings
   would own chronology and accounting, contradicting ADR 0011 and the module
   graph.
3. **Reuse Replay as the execution engine.** Rejected because K-line Replay is a
   presentation workflow and must never create orders or fills.
4. **Add a general strategy hierarchy immediately.** Rejected because one
   concrete starter behavior does not yet justify a polymorphic seam.
