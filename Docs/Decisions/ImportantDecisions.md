# Important Decisions

This is the project's single living record of design decisions that still
constrain current or approved implementation-ready work. Detailed behavior and
delivery status belong to the owning specifications; Git history preserves
obsolete chronology.

## Entry contract

Each retained decision contains only its current constraint, rationale,
important rejected alternatives, consequences, and owning specification.

## When an entry is required

Use the repository `grilling` workflow to resolve every ambiguous product,
compatibility, retention, or specification choice with the maintainer before
implementation.

Add or update an entry only when all three tests pass:

1. **Hard to reverse:** the choice materially constrains future work and would
   have a meaningful cost to change.
2. **Surprising without context:** a future contributor could not reliably
   infer the rationale from the owning spec and implementation.
3. **Real trade-off:** more than one reasonable answer existed and the rejected
   alternatives remain important to understand.

Apply these tests especially to dependency, cross-module public API, strategy
API, immutable snapshot, merge-gate, and module-graph changes. An implemented
merge-gate change always requires an entry. For another listed category that
does not pass all three tests, explain why an entry is unnecessary in the PR.

## Living decision documentation

**Current decision.** Maintain this one living document instead of a numbered,
append-only decision archive. Update a decision in place with its owning spec,
remove it when it stops constraining the project, and retain no superseded or
deprecated record solely for history.

**Why.** A small AI-heavy team needs durable rationale, but an append-only
archive accumulated stale authority, cross-reference churn, and contradictory
supersession metadata. One concise current record makes the applicable
constraint discoverable without treating archaeology as specification.

**Important rejected alternatives.** Issue or PR discussion alone was rejected
because the resolved rationale becomes difficult to discover. Code comments
were rejected because they do not own cross-module choices and disappear during
refactors. A numbered append-only archive was rejected because it preserves
obsolete documents as active reading material.

**Consequences.** Contributors must resolve ambiguity before implementation,
keep this record and the owning spec synchronized, and use Git history when
historical chronology is actually needed.

**Owning specification.**
[`Docs/Governance/AGENTS.md`](../Governance/AGENTS.md) §5.

## C++ and Qt desktop boundary

**Current decision.** Build the cross-platform desktop application in C++20
with Qt 6 Widgets, using CMake as build authority. Qt Charts remains a
development-only implementation; distribution uses a project-owned `QPainter`
chart behind `IChartView`. Backend modules remain plain C++ and Qt-independent;
Bindings translates backend values into Qt-facing view models.

**Why.** The application needs one Windows/macOS/Linux codebase, a performant
engine, mature desktop packaging, and backend tests that do not instantiate Qt.

**Important rejected alternatives.** Electron/Tauri was rejected because it
adds a web runtime and IPC around a C++ engine. QML was rejected because a
form-heavy application would add another language and theming surface.
Distributing Qt Charts under GPL/commercial terms and adding another third-party
chart dependency were rejected because they conflict with the intended
Apache-2.0 distribution or add another maintenance surface.

**Consequences.** Qt stays outside Core, Data, Strategy, Indicators, Engine,
Metrics, and Results. Qt Charts must not enter the Apache-2.0 distribution.
Chart implementations remain behind an interface so replacement does not force
an engine redesign.

**Owning specification.** [`Architecture.md`](../Specs/Architecture.md),
[`FrontendQt.md`](../Specs/FrontendQt.md), and
[`BuildDistribution.md`](../Specs/BuildDistribution.md).

## Engine and release-data authority

**Current decision.** The project-owned C++ engine alone owns chronology,
orders, fills, portfolio accounting, margin, metrics, and durable results.
Selectable Conditions compile to a typed C++ plan; planned Python Script
Strategies run as trusted local code in an isolated worker and submit commands
to the same engine. Release execution reads an immutable versioned snapshot,
uses checked fixed-point accounting, exposes `bte::core::Result<T>` at fallible
public C++ seams, and persists one transactional SQLite `.bteresult` container
whose deterministic identity is its canonical functional records and hash.

**Why.** One execution authority prevents semantic drift between authoring
modes and makes replay reproducible. Immutable data and content-addressed
references keep stored results portable without duplicating OHLCV. Fixed-point
values make accounting and persistence deterministic across platforms.

**Important rejected alternatives.** Embedding a second Python backtest engine
or the former Lua runtime was rejected because execution semantics would split.
Reading mutable DuckDB state in the release application was rejected because a
saved run could not identify immutable input. Copying all bars into every
result was rejected because it duplicates retained data. Treating physical
SQLite bytes as the determinism contract was rejected because storage layout is
non-functional.

**Consequences.** DuckDB and DataFetcher remain developer ingestion tools.
Python isolation protects application stability but is not a security sandbox.
Existing floating-point bars remain an incremental baseline. The former legacy
JSON replay summaries were removed rather than retained as a competing result
contract. A public data-bearing release stays
blocked until redistribution rights and a verified split manifest exist. A
separate updater is not a V1 dependency.

**Owning specification.** [`Overview.md`](../Specs/Overview.md),
[`Architecture.md`](../Specs/Architecture.md),
[`BackendCore.md`](../Specs/BackendCore.md),
[`DataLayer.md`](../Specs/DataLayer.md),
[`StrategyAuthoring.md`](../Specs/StrategyAuthoring.md),
[`EngineReplayPnL.md`](../Specs/EngineReplayPnL.md), and
[`BuildDistribution.md`](../Specs/BuildDistribution.md).

## Canonical result storage and lifecycle

**Current decision.** Results is a backend module depending only on Core, Data,
and SQLite. Each run receives an opaque per-run `ResultId` independent of its
functional content and is persisted as one schema-versioned SQLite
`.bteresult`. Its `canonicalResultHash` is SHA-256 over explicitly framed,
stably ordered functional identity and canonical records; Result ID, wall-clock
times, paths, catalog order, pacing, and SQLite physical bytes are excluded.
The container stores run and data-selection identities, ordered segment spans,
canonical records, terminal state, and eligible summaries, but references
retained immutable OHLCV segments instead of copying bars.

Writes use foreign keys, WAL, full synchronization, explicit record-batch
transactions, terminal finalization, WAL checkpoint, close, read-only reopen,
integrity/schema/hash validation, and same-filesystem no-clobber promotion from
Staging to Results. The lifecycle is Staging to a terminal state to Promoted;
restart recovery may promote a validated Interrupted diagnostic or quarantine
untrustworthy staging. Promoted results can move to Trash and restore; purge
releases segment references transactionally. Catalog state is rebuildable from
validated artifacts. Imports and migrations create newly validated copies and
never rewrite their source. SQLite is private to Results and is pinned to
version 3.53.4 through exact installed-package discovery.

**Why.** Stable functional framing makes deterministic replay portable without
pretending SQLite page layout is canonical. One independently portable artifact
supports validation, seeking, import, recovery, and user-visible lifecycle,
while retained content-addressed segments avoid duplicating market data.

**Important rejected alternatives.** JSON was rejected because it would need a
second ad-hoc protocol for integer encoding, ordering, indexing, and recovery.
One application-wide result database was rejected because artifacts must be
portable and independently promotable. Copying OHLCV into every result was
rejected because it duplicates immutable data. Treating abandoned staging as
Completed was rejected because a crash cannot establish terminal engine
semantics. Comparing physical SQLite bytes was rejected because page and
journal layout are non-functional.

**Consequences.** Public Results APIs expose typed identities, records, and
lifecycle operations rather than SQLite handles. Promotion and recovery require
more work than writing one file, but incomplete or corrupt artifacts cannot
masquerade as completed runs. Readers reject unknown schema, framing, numeric,
source-timeframe, or aggregation-policy versions and never fall back to mutable
data.

**Owning specification.** [`Architecture.md`](../Specs/Architecture.md),
[`DataLayer.md`](../Specs/DataLayer.md), and
[`EngineReplayPnL.md`](../Specs/EngineReplayPnL.md).

## Agent authority and repository layout

**Current decision.** `.agents/skills` is the only project-skill directory.
Root `AGENTS.md` points to the canonical governance playbook, and a host adapter
may exist only as a thin import/pointer that does not redefine policy. All
non-negotiable rules live in governance and are mechanically enforced where
practical. Project-owned directories and file stems use PascalCase; documented
external-tool names, entrypoints, `UnitTest_<Thing>` names, and domain-data
identifiers are narrow exceptions. Unit tests live under
`Tests/Unit/<Module>/`.

**Why.** A single skill source prevents host copies from drifting, while thin
adapters preserve discovery on hosts that do not load the common entry point.
Central hard rules cannot depend on optional skill activation. Predictable path
and test layout makes repository concepts easier to find and audit.

**Important rejected alternatives.** Removing every host adapter was rejected
because discovery is not uniform. Duplicating or symlinking skills per host was
rejected because copies drift and Windows symlink behavior varies. Leaving hard
rules only in task-triggered skills was rejected because activation can be
missed. Documenting path conventions without enforcement was rejected because
the full-tree checker can prevent regressions.

**Consequences.** Adapters stay deliberately small, skills may explain but
never weaken governance, and contextual design judgments remain reviewer work.
The standards checker owns the mechanical path and test-layout rules and keeps
only explicit conventional-name exceptions.

**Owning specification.**
[`Docs/Governance/AGENTS.md`](../Governance/AGENTS.md) and
[`CiDevFlow.md`](../Specs/CiDevFlow.md).

## Incremental strategy and engine seams

**Current decision.** Add executable behavior as narrow vertical slices behind
backend seams. The compatibility starter Backtest remains a limited
function-shaped engine operation rather than a speculative general hierarchy.
Selectable Conditions are a validated typed plan owned by Strategy, use
chronology-safe Indicators, and produce signals that only Engine can turn into
next-actual-bar orders. Frontend and Bindings remain presentation adapters.

**Why.** A complete horizontal engine design would create many unexercised
interfaces before a usable path exists. Putting policy in Qt, Bindings, or
Engine would duplicate authoring semantics and violate module ownership. A
typed plan can serve the UI now and later support persistence and equivalence
tests.

**Important rejected alternatives.** Implementing the complete engine before a
page was rejected because feedback would be delayed. Executing trades in the
view model or Replay was rejected because presentation would own chronology and
accounting. Adding a general strategy hierarchy for one starter behavior was
rejected because it had no second real caller. Generating Python for condition
execution was rejected because the typed C++ plan is authoritative.

**Consequences.** The current starter and long-only condition paths remain
explicitly limited and regression-tested. New abstractions must arrive with a
real second caller or adapter and complete contract tests. Indicator warm-up
never fabricates values, and a signal cannot fill on the bar that produced it.

**Owning specification.** [`Architecture.md`](../Specs/Architecture.md),
[`StrategyAuthoring.md`](../Specs/StrategyAuthoring.md),
[`Indicators.md`](../Specs/Indicators.md), and
[`EngineReplayPnL.md`](../Specs/EngineReplayPnL.md).

## Layered merge-blocking quality gates

**Current decision.** Every submitted revision must pass the full-tree project
standards audit, all registered tests, complete-project clang-tidy/cppcheck/
IWYU/scan-build analysis, separate ASan/UBSan/LSan and TSan jobs, and 98 percent
changed-line plus 90 percent changed-branch coverage. `RunQuality.sh` is the
matching local entry point. External actions use immutable commit SHAs,
checkout credentials are not persisted, and coverage dependencies use an exact
hash-locked closure. Generated reports stay untracked and CI publishes
per-commit evidence.

**Why.** A diff-only standards check can hide existing violations, while
changed-unit analysis cannot prove the submitted toolchain still accepts the
whole project. Diff coverage measures the behavior introduced by a change
without redistributing historic test debt. Independent analyzers, sanitizers,
and line-oriented rules catch different defect classes; no one layer can claim
semantic completeness.

**Important rejected alternatives.** A standards suppression baseline was
rejected because the correctable tree can remain fully compliant. Whole-project
coverage thresholds were rejected because they obscure the submitted change.
Changed-only or weekly-only static analysis was rejected because a green PR
would lack whole-project evidence. Mutable action tags, persisted checkout
credentials, and unhashed dependency resolution were rejected as avoidable
supply-chain exposure. Committed generated reports were rejected because they
can be stale and create merge noise. Treating regular expressions as proof of
design, performance, or ownership was rejected because those judgments require
AST/runtime evidence and review.

**Consequences.** Tightening a direct rule requires cleaning the complete
baseline and adding focused checker tests. Quality jobs add build time and tool
pin maintenance. GPL cppcheck remains CI/developer-only and is not distributed
with the application. Gate changes require an approved update to this entry,
the owning specification, focused tests, and review.

**Owning specification.** [`CiDevFlow.md`](../Specs/CiDevFlow.md).

## Maintenance rules

- Update the applicable entry in place with its owning specification.
- Remove an entry when it no longer constrains the project.
- Keep dependency version and license facts in
  [`Dependencies.md`](Dependencies.md).
- Do not create numbered, append-only, superseded, or deprecated decision
  records.
