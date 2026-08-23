# Team Ownership and Product Pillars

> **Status:** Organizational guidance. The canonical specifications define
> behavior. Actual human assignments are **Blocked** pending a maintainer
> decision; the labels below name responsibility areas, not assigned people.

## Working rule

Owners define narrow contracts and test against frozen or in-memory fixtures so other workstreams can proceed independently. An owner label indicates accountability, not exclusive permission to contribute.

## Topic owners

- **Data acquisition and cleansing:** `DataFetcher/`, source validation, calendar alignment, split provenance, and release snapshot inputs.
- **Data contract and read model:** managed-snapshot schema, manifests, bars, calendars, read APIs, and drift detection.
- **C++ engine and results:** order lifecycle, fills, short selling, portfolio,
  metrics, deterministic Batch/Paced Backtest behavior, and result storage.
- **Strategy authoring:** Selectable Conditions, explicit Python template/API/worker, validation, and indicator bindings.
- **Desktop client:** Qt tabs, charts, strategy editors, results, replay, accessibility, and user-facing diagnostics.
- **CI and quality:** actual workflows, public-behavior tests, fixtures, standards, and truthful gate documentation.
- **Release and packaging:** supported platforms, managed Python runtime, immutable data snapshot, licensing, signing, and installers.

## Product pillars

### Backtest and strategy authoring

- Strategy owner defines conditions and Python artifacts.
- Engine owner executes the canonical typed strategy plan and produces results.
- Data owner supplies immutable `MarketSlice` inputs.
- Desktop owner provides validation, run controls, and result presentation.

### K-line replay

- Replay uses stored result events and the exact managed-data identity from the original run.
- Batch and Paced Backtest execution share the same C++ fill and accounting
  implementation. K-line Replay only presents their persisted outcome.
- The desktop owner renders candles, fills, positions, cash, equity, and diagnostics.

### Stock screening

- Screening uses the managed snapshot and versioned predicates.
- Conditions share operator definitions with strategy authoring where semantics match.
- Python and AI screening remain planned until promoted in canonical specs.
- Valuation models and the existing HTML prototype are non-normative proposals.

## Cross-cutting obligations

- Follow [`Specs/README.md`](Specs/README.md), [`Governance/AGENTS.md`](Governance/AGENTS.md), and [`DefinitionOfDone.md`](DefinitionOfDone.md).
- Public behavior requires positive, negative, and meaningful boundary unit tests.
- Every bug fix or behavior change requires a regression test.
- Data, strategy, engine, results, and replay compatibility changes are
  coordinated through their owning specs and, when the three-part governance
  threshold applies, the living important-decisions document.
- Planned checks must be listed as not implemented until automation exists.

Ownership changes update this document; behavioral changes update the owning spec.
