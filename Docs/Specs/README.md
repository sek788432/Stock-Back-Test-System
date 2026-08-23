# Specifications

These files define the intended C++/Qt desktop backtester. Read `00` first, then the owning subsystem spec. A status label in each document distinguishes implemented behavior from accepted design and future work.

## Canonical reading order

| Spec | Scope |
|---|---|
| [`00Overview.md`](00Overview.md) | Product scope and end-to-end flow |
| [`01Architecture.md`](01Architecture.md) | Module boundaries, dependency direction, threading, and errors |
| [`02FrontendQt.md`](02FrontendQt.md) | Qt UI, charts, replay, and view models |
| [`03BackendCore.md`](03BackendCore.md) | Core value types, fixed-point quantities, orders, and results |
| [`04DataLayer.md`](04DataLayer.md) | Frozen managed snapshot, bars, calendar, splits, and identity |
| [`05StrategyAuthoring.md`](05StrategyAuthoring.md) | Selectable Conditions and explicit Python strategy contract |
| [`06Indicators.md`](06Indicators.md) | Deterministic streaming indicators |
| [`07EngineReplayPnL.md`](07EngineReplayPnL.md) | C++ engine, fills, short selling, accounting, results, and replay |
| [`08PluginSystem.md`](08PluginSystem.md) | Future native plugin boundary; not a V1 dependency |
| [`09BuildDistributionLauncher.md`](09BuildDistributionLauncher.md) | Builds, supported releases, runtime/data packaging, and current gaps |
| [`10CiDevFlow.md`](10CiDevFlow.md) | Actual and planned merge gates and public-behavior test policy |
| [`11StockScreenerKLineProduct.md`](11StockScreenerKLineProduct.md) | User workflows for backtests, replay, strategy modes, and screening |
| [`12AiActionRouter.md`](12AiActionRouter.md) | Future provider-neutral AI candidate import and acceptance boundary |

The [`StockScreening/`](StockScreening/README.md) folder contains non-normative proposals and a UI prototype. It does not override `03`–`07`, `11`, or `12`.

## Accepted foundations

- The canonical backtest engine is project-owned C++, not Zipline, Backtrader, vn.py, or another framework.
- Selectable Conditions become a typed plan executed by the C++ engine.
- Explicit Python runs in a fresh trusted-local worker and communicates with C++ through control pipes plus read-only shared memory. It uses the same C++ execution/accounting engine.
- V1 reads the frozen managed snapshot derived from `StockData/Extracted`; arbitrary user-data import and runtime providers are excluded.
- Batch and Paced Backtest execution use the same project-owned C++ engine and
  must produce identical canonical records for identical inputs.
- K-line Replay presents a versioned Backtest Result plus its exact data
  identity; it never reruns the Strategy, Python worker, or C++ engine.
- Native plugins and AI import are planned seams, not implemented features.
- Project source uses Apache-2.0; third-party code and market data retain their own terms.
- Tests are required for every public behavior: positive, negative, and meaningful boundary cases. Bug fixes and behavior changes require regression tests.

## C++ engine contract ownership

| Concern | Owning contract |
|---|---|
| Module ownership, processes, and seams | [`01Architecture.md`](01Architecture.md) |
| Fixed-point values, errors, and run status | [`03BackendCore.md`](03BackendCore.md) |
| Immutable market inputs, calendar, and splits | [`04DataLayer.md`](04DataLayer.md) |
| Strategy commands and the Python worker seam | [`05StrategyAuthoring.md`](05StrategyAuthoring.md) |
| Event order, fills, accounting, margin, metrics, scheduling, and `.bteresult` | [`07EngineReplayPnL.md`](07EngineReplayPnL.md) |
| Backtest, Paced Backtest, and K-line Replay user workflows | [`11StockScreenerKLineProduct.md`](11StockScreenerKLineProduct.md) |
| Determinism and required verification | [`10CiDevFlow.md`](10CiDevFlow.md) |

Overlapping documents summarize these concerns but do not redefine them.

## Status language

- **Implemented:** code and automated verification exist in the repository.
- **Accepted design:** normative behavior for implementation, but not necessarily shipped.
- **Planned / not implemented:** not available and not an active CI gate.
- **Proposal / non-normative:** exploration only; do not implement without promotion into a canonical spec and, when required by governance, the living important-decisions document.

## Change rules

- Change the owning spec in the same PR as behavior.
- Resolve material ambiguity with the maintainer. Update the living
  important-decisions document when the governance threshold applies.
- Engine-semantic changes require deterministic fixture updates and regression tests.
- Never describe a planned check as merge-blocking until the workflow actually enforces it.
- Temporary investigations belong in ignored `Docs/Reviews/` and must be folded into canonical docs before merge.
