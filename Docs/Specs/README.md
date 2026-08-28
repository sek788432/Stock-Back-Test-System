# Specifications

Canonical specifications use descriptive filenames because their scope and
organization evolve. Filenames do not encode an ordering or permanent ID.
Start with the overview, then read the owning subsystem specification.

## Canonical specifications

| Specification | Scope |
|---|---|
| [`Overview.md`](Overview.md) | Product scope, delivery status, and end-to-end flow |
| [`Architecture.md`](Architecture.md) | Module boundaries, dependency direction, processes, threading, and errors |
| [`FrontendQt.md`](FrontendQt.md) | Qt UI, chart presentation, strategy and result libraries, and view models |
| [`BackendCore.md`](BackendCore.md) | Core values, fixed-point quantities, errors, and statuses |
| [`DataLayer.md`](DataLayer.md) | Immutable snapshots, segments, calendars, splits, and retention |
| [`StrategyAuthoring.md`](StrategyAuthoring.md) | Selectable Conditions and trusted-local Python strategy contracts |
| [`Indicators.md`](Indicators.md) | Deterministic streaming indicators |
| [`EngineReplayPnL.md`](EngineReplayPnL.md) | Execution, fills, accounting, metrics, durable results, and replay |
| [`BuildDistribution.md`](BuildDistribution.md) | Supported release artifacts, packaging, and release blockers |
| [`CiDevFlow.md`](CiDevFlow.md) | Implemented and planned checks and public-behavior test policy |
| [`BacktestReplayProduct.md`](BacktestReplayProduct.md) | User workflows for strategy storage, backtests, results, and replay |

The [`StockScreening/`](StockScreening/README.md) subtree is a temporarily
protected, non-canonical proposal area. It is excluded from this cleanup phase,
does not override a canonical specification, and requires a separate maintainer
review before issue #53 can reach final acceptance.

## Accepted foundations

- The project-owned C++ engine is the only execution and accounting authority.
- Selectable Conditions execute as a typed C++ plan.
- Python strategies run as trusted local code in a fresh managed worker and
  submit commands to that same engine.
- V1 reads immutable project-managed data derived from
  `StockData/Extracted`; user pricing-data import is unsupported.
- Batch and Paced Backtest modes must produce identical canonical records for
  identical functional inputs.
- K-line Replay presents an existing Backtest Result and never reruns a
  Strategy, Python worker, or execution engine.
- Native plugins, AI-assisted authoring, direct provider integration, and
  advanced multi-run concurrency are outside the accepted product scope.
- Tests are required for every public behavior: positive, negative, meaningful
  boundaries, and regressions for defects or intentional behavior changes.

## Status language

- **Implemented:** code and automated verification exist in this repository.
- **Accepted design:** implementation-ready contract that has not necessarily
  shipped.
- **Planned / not implemented:** retained contract that is not currently
  available and is not a merge gate unless the CI specification says it is.
- **Blocked:** must not ship until the named prerequisite is satisfied.
- **Proposal / non-canonical:** exploration that cannot authorize
  implementation.

## Change rules

- Update the owning specification in the same PR as behavior.
- Resolve material ambiguity with the maintainer before implementation.
- Update the living important-decisions document only when governance's
  hard-to-reverse, surprising, and real-trade-off tests all apply.
- Never describe a planned check as merge-blocking until the workflow requires
  it.
- Temporary investigations belong in ignored `Docs/Reviews/`; fold conclusions
  into canonical documents and remove the temporary material before merge.
