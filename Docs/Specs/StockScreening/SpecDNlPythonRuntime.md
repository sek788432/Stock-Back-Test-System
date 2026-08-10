# Spec D — Future Python and AI Screening

> **Status:** Non-normative proposal. Python screening and AI candidate import are not implemented or accepted for V1.

## Constraints inherited from canonical specs

- Screening cannot introduce a second portfolio, fill, or accounting engine.
- Any Python execution must reuse the managed worker, template/API versioning, diagnostics, process isolation, control pipes, and read-only shared-memory boundary defined by `../05StrategyAuthoring.md`.
- Inputs come from the managed snapshot defined by `../04DataLayer.md`; unrestricted local-data import is excluded.
- AI output is a candidate artifact requiring validation, preview, and explicit acceptance under `../12AiActionRouter.md`.
- The application stores no provider credential and does not call a named AI vendor in V1.
- A trusted-local Python worker is not a security sandbox. Documentation must not promise protection from malicious local scripts.

## Open design work

- Decide whether Python screening provides enough value beyond typed Selectable Conditions.
- Define a columnar, versioned, read-only screening input schema.
- Define deterministic output ordering, missing-value handling, time limits, cancellation, and maximum result size.
- Reconcile screening predicates with strategy-condition operators.
- Define a provider-neutral AI candidate envelope only if AI import is implemented.

## Required verification before promotion

- Positive, negative, and meaningful boundary tests for every public screening behavior.
- Regression tests for every bug fix or behavior change.
- Determinism tests for identical snapshot, predicate, and configuration.
- Process-crash, timeout, malformed-output, cancellation, and oversized-result tests.
- No-credential and explicit-acceptance UI tests for AI candidates.

Until these decisions are promoted into canonical specs, the prototype must not be used as an implementation contract.
