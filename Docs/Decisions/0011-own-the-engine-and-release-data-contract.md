# 0011 — Own the engine and release-data contract

- **Status**: Accepted
- **Date**: 2026-08-09
- **Deciders**: repository maintainer
- **Supersedes**: 0003, 0005
- **Clarifies**: 0002
- **Superseded by**: —

## Context

ADR 0002 correctly chose C++20 and Qt for the desktop application, but it did not decide which runtime owns backtesting. ADR 0003 selected Rule + Lua authoring. Later specs mixed Lua, Python, DuckDB-at-runtime, CSV fallback, floating-point accounting, and JSON results. That leaves both maintainers and AI agents without one executable design.

## Decision

- The project-owned C++ engine is the only authority for chronology, orders, fills, portfolio accounting, margin, metrics, and persistence.
- Selectable Conditions compile to a typed C++ strategy plan. Python Script Strategies run as trusted local code in a fresh isolated worker and submit commands to the same C++ engine. Python is not a second engine and is not described as a security sandbox.
- The release application reads only an immutable, versioned snapshot built from `StockData/Extracted`. DuckDB and `DataFetcher/` remain developer ingestion and verification tools; users cannot rebind the release application to arbitrary data in V1.
- Authoritative accounting uses checked fixed-point `Price`, `Quantity`, `Money`, and `Rate` types. Floating point is limited to analytics, display, and the ergonomic Python-facing view; submitted values are normalized at the C++ seam.
- Fallible public C++ interfaces return `bte::core::Result<T>`, whose error type is fixed by the class. The spelling `Result<T, Error>` is not part of the project interface.
- Each run persists to a versioned SQLite `.bteresult` container. Results store canonical events and content-addressed snapshot references rather than duplicating OHLCV. Determinism is measured by canonical functional records and `canonicalResultHash`, not physical SQLite bytes.
- A public release is blocked until the project records market-data redistribution rights and provides a verified, redistribution-cleared split manifest. Split events are never inferred from price gaps; dividends remain excluded and disclosed for V1.
- V1 ships as a normal signed installer with its managed runtime and data snapshot. The separate self-updating launcher from ADR 0005 is deferred and is not a V1 architectural dependency.

## Consequences

- ADR 0003's Lua runtime, sol2 binding, and Rule/Lua parity contract are retired from the target architecture.
- ADR 0005's mandatory separate launcher, side-by-side application versions, and automatic update contract are retired from V1. A later updater requires a new ADR.
- The Python worker, release-snapshot builder, fixed-point migration, and `.bteresult` store remain planned until code and tests land; specs must not describe them as implemented.
- Existing CSV replay, `double` domain fields, and JSON result snapshots are an implemented baseline to migrate, not the release contract.
- Release snapshots and runtime profiles must be retained while saved results reference them.

## Alternatives considered

1. **Embed Zipline or another Python backtest engine.** Rejected because two engines would duplicate execution semantics and weaken deterministic replay.
2. **Keep Lua as the script language.** Rejected because the accepted product language is Python and its data-science ecosystem is the expected user environment.
3. **Read DuckDB directly in the release app.** Rejected because mutable local databases undermine release reproducibility and portable result references.
4. **Copy all bars into every result.** Rejected because large runs would duplicate immutable data and make retention expensive.
