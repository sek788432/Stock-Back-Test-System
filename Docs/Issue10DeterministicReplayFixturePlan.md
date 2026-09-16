# Backtest Result K-Line Replay Implementation Plan

> **Implementation status (2026-09-16):** Tasks 1–7 and the acceptance
> checklist are implemented for the plan's declared current single-symbol
> Engine scope. Unsupported future record families remain explicit capability
> flags, as required by §3 and §7; they are not fabricated.

> **For agentic workers:** REQUIRED SUB-SKILL: use
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan task by task. Track work
> with the checkboxes below.

**Goal:** Persist every supported Backtest as a canonical `.bteresult` and let
the user open or select that result in K-line Replay to inspect its exact
hourly/daily candles, volume, fills, and portfolio history.

**Architecture:** The existing C++ Engine remains the sole execution and
accounting authority. A new Results module transactionally records each run and
references immutable Data Segments; Bindings opens those records for a
presentation-only Replay model, and the application shell passes only a Result
ID between Backtest and Replay.

**Tech stack:** C++20, Qt 6.8+ Widgets/Charts/Concurrent, SQLite, CMake, GoogleTest,
Qt Test.

**Authoritative specs:** [Overview](Specs/Overview.md),
[Architecture](Specs/Architecture.md), [Frontend](Specs/FrontendQt.md),
[Data](Specs/DataLayer.md), [Engine/Results](Specs/EngineReplayPnL.md),
[CI](Specs/CiDevFlow.md), [Product](Specs/BacktestReplayProduct.md), and
[ADR 0011](Decisions/ImportantDecisions.md#canonical-result-storage-and-lifecycle).

## Global Constraints

- Every valid Backtest automatically targets one staged `.bteresult`; there is
  no manual Save operation.
- The Backtest footer action is **Open in Replay** and is enabled only after a
  result has been finalized and promoted.
- Strategy execution remains hourly (`ohlcv-1h`). Replay displays **Hourly** or
  **Daily (UTC, aggregated)** only.
- Replay never calls Strategy hooks, Python, indicator scheduling, order
  evaluation, fill creation, or Engine execution.
- Results reference exact immutable Data Segments; they never copy OHLCV or
  fall back to mutable/newer CSV data.
- Engine/result financial records remain checked fixed-point values. Conversion
  to display values happens only in Bindings.
- All fallible public backend operations are `[[nodiscard]]` and return
  `bte::core::Result<T>`; exceptions do not cross module seams.
- Record/list/open/hash/data-validation work runs off the Qt UI thread through
  owned cancellable workers and queued immutable delivery.
- Public release with project-managed market data remains **Blocked** until the
  redistribution-rights and verified split-manifest gates are cleared.
- Legacy JSON summaries remain isolated and are never presented or converted
  as replayable Backtest Results.

---

## 1. Scope and Current Baseline

[Issue #10](https://github.com/sek788432/Stock-Back-Test-System/issues/10)
remains the final small, offline determinism fixture. The production
prerequisites below belong in separate linked issues/PRs; they must not be
hidden inside the issue #10 test change.

Verified current behavior:

- Backtest returns only final accounting and ordered fills in memory.
- Replay loads tracked hourly CSV data and can aggregate it by UTC date.
- Replay is symbol/date driven; its portfolio values are placeholders.
- Unsupported Replay schemas can currently receive hourly bars.
- The chart renders candles, but its marker operation is empty and it has no
  rendered volume series despite older status wording.
- Legacy result summaries are JSON files owned by Engine, not canonical
  `.bteresult` artifacts.
- Backtest and Replay tabs have no Result ID navigation seam.

This plan implements result-backed Replay for the current single-symbol Engine.
Persisted records are symbol-keyed and retain an ordered universe so later
multi-symbol work does not require replacing the result identity model.

## 2. Product and Replay Contract

### Backtest lifecycle

1. Validate the Run Configuration and immutable Data Selection Identity.
2. Allocate an opaque per-run Result ID, separate from
   `canonicalResultHash`.
3. Create same-filesystem staging before Engine execution records begin.
4. Append canonical records in stable order as the Engine runs.
5. Finalize as `Completed`, `Failed`, `Canceled`, or `Incomplete`; restart
   recovery converts trustworthy abandoned staging to `Interrupted`.
6. Reopen, validate, and atomically promote the result before catalog
   visibility.
7. Enable **Open in Replay** with the promoted Result ID.

Independent identical runs retain distinct Result IDs but share the same
`canonicalResultHash`. A partial or corrupt file is never listed as a valid
result.

### Replay cursor

- Opening a non-empty result immediately reveals its first Hourly candle (or
  first Daily bucket), volume, persisted fills, and corresponding post-slice
  portfolio state. The user never lands on an empty pre-record frame.
- Each subsequent Hourly step advances by exactly one referenced candle and
  reveals its volume, all persisted same-timestamp records/fills in stable
  order, and the post-slice portfolio checkpoint after the canonical event
  sequence.
- Each subsequent Daily step advances by one UTC calendar-date bucket using
  `utcCalendarDayV1`: first open, maximum high, minimum low, last close, summed
  checked volume, all fills, and the last post-slice checkpoint in the bucket.
- A first, last, or diagnostic UTC bucket that lacks a full source day remains
  visible and is labelled **Partial**; Replay never drops executed bars.
- Diagnostic Replay stops at the last trustworthy processed record and shows
  its structured terminal reason. It does not reveal unprocessed future bars.
- Hourly/Daily choice is presentation state. The aggregation-policy version is
  compatibility metadata and is excluded from `canonicalResultHash`.

### Catalog and errors

- Results sort by saved time descending, then Result ID.
- Corrupt, incompatible, and missing-data entries never block valid results.
  They remain visible but disabled with the exact structured reason; Trash is a
  separate catalog state and is excluded from the normal Replay selector.
- Selecting a result stops playback, cancels the prior load, rejects stale
  completion, validates result/data hashes, resets the cursor to record zero,
  and immediately presents that first record. Empty valid results remain in
  their explicit empty state.
- Empty catalog, empty valid result, typed failure, and cancellation remain
  distinct states.

### Performance envelope

- Support the current Data history ceiling of 10,000 hourly bars per symbol.
- Render at most 500 candles/volume bars at once.
- Seek and step through indexed persisted records; never replay from record zero
  or rebuild an unbounded chart on every step-back.
- Verify 1,000 catalog entries and document reproducible open, seek, maximum
  playback, and resident-memory measurements. Performance claims require
  measured evidence, not a hard-coded machine-independent latency promise.

## 3. Persisted Result Contract

The first schema implements the accepted record families for the capabilities
the current Engine can truthfully produce. Unsupported subsystems have explicit
capability metadata and no fabricated records.

| Area | Required content |
| --- | --- |
| Identity | Result ID, schema/Engine/Strategy/numeric-policy versions, canonical hash |
| Run | ordered universe, symbol-keyed records, half-open range, capital, costs, Strategy artifact/source hash |
| Data | snapshot/calendar/split identities plus ordered segment IDs, hashes, and selected row spans |
| Status | completed/failed/canceled/interrupted/incomplete and structured reason |
| Events | typed stable sequence for orders, fills, trades, warnings, logs, and applicable indicator/corporate-action/margin records |
| Portfolio | post-slice cash, restricted cash where supported, positions, market value, equity, realized/unrealized P&L, costs |
| Summary | eligible completed metrics; diagnostics never expose valid final performance metrics |
| Metadata | creation/save time and local path excluded from canonical functional hashing |

Hash framing, integer encoding, record ordering/tie-breakers, absent-field rules,
SQLite journal/durability policy, collision handling, and reader compatibility
must be fixed by the ADR before schema code begins.

## 4. Planned File Map

| Area | Planned files/responsibility |
| --- | --- |
| Decisions/docs | New result/replay ADR under `Docs/Decisions/`; update Specs 00/01/02/04/07/11, `Docs/Decisions/Dependencies.md`, and `Docs/KLineReplayRoadmap.md` |
| Build | Root `CMakeLists.txt`, new pinned `vcpkg.json`, new `Src/Backend/Results/CMakeLists.txt`, affected module/test CMake files |
| Data | `Src/Backend/Data/Include/Bte/Data/` and `Private/` for immutable snapshot identity, segment reader, row-span selection, and retention |
| Snapshot build | A separate release-build target finalized by the ADR; application runtime remains read-only |
| Results | New `Src/Backend/Results/Include/Bte/Results/` and `Private/` for canonical record values, run recording, store/catalog, recovery, import, Trash, and purge |
| Engine | Existing `Backtest.h/.cpp` for canonical post-slice records and Results recording; remove result persistence from legacy Engine ownership |
| Bindings | Existing Backtest/Replay bindings plus focused result catalog/open adapters using immutable queued values |
| Frontend/App | Backtest footer CTA, result-driven Replay setup, chart layers, and shell-owned Result ID navigation |
| Tests | New `Tests/Unit/Results/`; extend Data, Engine, Bindings, Frontend, App-shell, fixture, and CMake registration |

Exact new public type names and SQLite tables are owned by the ADR/schema review;
implementation must not expose SQLite or Qt types across backend module seams.

## 5. Delivery Tasks

### Task 1 — Freeze architecture, schema, and dependency decisions

**Files:** `Docs/Decisions/`, Specs 00/01/02/04/07/11,
`Docs/Decisions/Dependencies.md`, `Docs/KLineReplayRoadmap.md`, root build files.

- [x] Write the ADR covering Results module ownership, automatic lifecycle,
      Result ID/hash separation, canonical record framing, SQLite durability,
      recovery state machine, immutable-data retention, compatibility, and
      legacy isolation.
- [x] Pin SQLite through the repository manifest, record exact version/license,
      and wire the planned Results target without relying on an untracked system
      installation.
- [x] Correct delivery-status text where it claims a rendered volume pane or
      complete result behavior that the checkout does not contain.
- [x] Link prerequisite issues and retain issue #10 as the final fixture only.
- [x] Validate all changed links and status labels.
- [x] Commit as `docs(results): define persisted replay architecture`.

**Exit:** reviewers can determine every persisted identity, record family,
ordering rule, lifecycle transition, compatibility error, and module dependency
without consulting implementation guesses.

### Task 2 — Build immutable snapshots and exact data selection

**Files:** Data public/private files, snapshot-builder target, Data tests and
fixtures, Data CMake registration.

- [x] Add failing unit/contract tests for deterministic segment generation,
      manifest/hash validation, half-open selection, exact ordered row spans,
      UTC boundaries, missing/corrupt/out-of-order segments, and cancellation.
- [x] Implement release-built content-addressed hourly segments and a read-only
      reader returning bars plus immutable Data Selection Identity.
- [x] Add transactional reference acquisition/release, Trash restore, and purge
      race tests so a promoted result cannot lose required segments.
- [x] Migrate Backtest data selection away from mutable path discovery before
      any result writer is enabled.
- [x] Run `bte_data_tests` and the new contract tests; verify deliberate manifest
      mutation fails closed as `DataSnapshotUnavailable`.
- [x] Commit as `feat(data): add immutable backtest data identity`.

**Exit:** the exact bars used before execution can be reopened later solely from
persisted identities and row spans.

### Task 3 — Add the transactional Results module

**Files:** new Results module, Results tests, root/module/test CMake files;
legacy `ResultSnapshotStore` remains isolated.

- [x] Add failing tests for begin/append/finalize/promote, Result ID collision,
      canonical hashing, catalog ordering, schema/version mismatch, import,
      recovery, corrupt staging, Trash/restore/purge, and reference counts.
- [x] Implement the deep Results interface: start one run, record ordered typed
      batches, finalize/promote, list summaries, open one result, and manage its
      lifecycle while hiding SQLite details.
- [x] Reopen and validate before same-filesystem no-clobber promotion; incomplete
      artifacts never become catalog-visible.
- [x] Fault-inject after schema creation, record transactions, hash finalization,
      close, promotion, and catalog visibility; verify restart recovery yields
      `Interrupted` only when trustworthy records exist.
- [x] Prove canonical hash independence from save time, local path, SQLite page
      layout, and catalog order.
- [x] Run the Results unit and contract test targets.
- [x] Commit as `feat(results): add transactional backtest result store`.

**Exit:** an automatically recorded result survives restart, validates, lists,
opens, and retains its exact data without fabricated fields.

### Task 4 — Emit authoritative Engine records

**Files:** Engine Backtest interface/implementation, Results integration,
Engine tests.

- [x] Add failing tests for post-slice portfolio checkpoints, multiple
      same-timestamp records, no-signal, one-bar, cancellation, operational
      failure, incomplete final mark, and deterministic repeat execution.
- [x] Separate pre-run validation errors (no Backtest starts) from terminal run
      statuses that finalize diagnostic results.
- [x] Emit fixed-point canonical orders, fills, portfolio/cost records, warnings,
      and stable sequence IDs through the Results recording seam.
- [x] Ensure every processed hourly bar has one post-slice checkpoint after the
      accepted event sequence; Replay never derives accounting from fills.
- [x] Compare Batch/Paced functional records and hashes for identical inputs.
- [x] Run `bte_engine_tests` under normal and sanitizer presets.
- [x] Commit as `feat(engine): record canonical backtest timelines`.

**Exit:** each supported Backtest automatically finishes with a promoted
completed or diagnostic Result ID and authoritative Replay records.

### Task 5 — Add asynchronous result-backed Bindings

**Files:** Backtest and Replay Bindings headers/implementations, Bindings tests
and CMake registration.

- [x] Add failing tests for list/open/cancel, stale completion, corrupt entries,
      missing data, unsupported timeframe/policy, Hourly stepping, UTC Daily
      aggregation, immediate first-frame presentation, partial buckets, seek,
      and diagnostic truncation.
- [x] Expose immutable result summaries and a result-backed Replay presentation
      model; keep backend fixed-point values authoritative until display
      conversion.
- [x] Replace replay-from-zero rewind with indexed cursor/window access and
      precomputed daily bucket indices.
- [x] Run store/data validation on owned cancellable workers; deliver only
      queued immutable values and ignore obsolete request generations.
- [x] Remove schema fallback from both Backtest and Replay loading seams:
      execution accepts hourly only; presentation accepts Hourly/Daily only.
- [x] Run the Bindings test targets, including rapid result switching and view
      destruction during open.
- [x] Commit as `feat(bindings): expose persisted result replay`.

**Exit:** Bindings can open a Result ID and deterministically present bounded
Hourly/Daily frames without invoking Engine execution.

### Task 6 — Connect Backtest, Replay, and the application shell

**Files:** Backtest/Replay Frontend, chart interface/adapter, MainWindow, Qt and
shell tests.

- [x] Add failing Qt tests for **Open in Replay** enablement, accessibility,
      shell navigation, exact ID selection, selector states, keyboard controls,
      immediate first-candle presentation, fills, volume, portfolio, trade
      rows, seek, and selection while playing.
- [x] Add the Backtest footer CTA; it never saves and is enabled only after
      automatic result promotion.
- [x] Make MainWindow own the narrow Result ID navigation request; tabs never
      find or manipulate each other.
- [x] Replace primary symbol/date Replay setup with result selection. Keep any
      bar-only preview explicitly separated and remove placeholder accounting
      from result mode.
- [x] Extend the chart interface once for bounded candle/volume/marker frames;
      render hourly source volume and checked Daily sums on the shared window.
- [x] Render buy/sell markers with color plus shape, accessible fill details,
      synchronized trade rows, and **Partial** Daily labels.
- [x] Verify no more than 500 bars render and maximum playback batches UI
      updates without blocking the event loop.
- [x] Run all Qt tests offscreen and commit as
      `feat(replay): visualize persisted backtest results`.

**Exit:** users can open the just-finished result or select any available result
and inspect exact candles, volume, fills, and persisted portfolio state.

### Task 7 — Land issue #10 deterministic production-path fixture

**Files:** `Tests/Fixtures/`, end-to-end test source, `Tests/CMakeLists.txt`,
roadmap/issue traceability.

- [x] Commit a small synthetic immutable hourly snapshot containing UTC-day
      boundaries, multiple fills, a partial bucket, and non-trivial volume.
- [x] Build one literal canonical result through the production Results writer
      and implement `ReplayFixture_replaySnapshotsMatchExpectedSequence`
      through the real snapshot → `.bteresult` → catalog → Replay seams.
- [x] Implement `ReplayFixture_replayTwiceProducesSameSnapshots`; assert
      identical literal presentation sequences from the same immutable result.
- [x] Add a separate integration assertion that two identical Backtests produce
      distinct Result IDs and identical canonical records/hashes; do not present
      that assertion as issue #10's original test-only acceptance criterion.
- [x] Assert exact candles, volume, stable fill order, post-slice portfolio,
      progress, Daily aggregation, and absence of Engine/Strategy calls during
      Replay.
- [x] Deliberately mutate one production behavior and confirm the focused test
      fails before restoring the source.
- [x] Run the focused target, complete CTest presets, `./RunTest.sh`, and
      `./RunQuality.sh --base <base-revision> --head HEAD` on the exact committed
      revision.
- [x] Commit as `test(replay): add deterministic K-line replay fixture`.

**Exit:** issue #10 proves stable behavior through the actual persisted-result
workflow, offline and independent of wall-clock time.

## 6. Acceptance Checklist

- [x] Every valid supported Backtest automatically produces one validated
      `.bteresult`; crash recovery never labels an incomplete file Completed.
- [x] **Open in Replay** opens the exact promoted Result ID without saving again.
- [x] Opening a non-empty result immediately shows its first Hourly candle or
      Daily bucket with synchronized volume, fills, and post-slice portfolio;
      the next Step advances exactly once.
- [x] Replay can select completed and diagnostic results and isolates unavailable
      catalog entries.
- [x] Referenced hourly candle records are identical to the canonically
      normalized immutable input selected before execution.
- [x] Daily Replay follows `utcCalendarDayV1`, sums volume, includes every fill,
      uses the last post-slice checkpoint, and labels truncated buckets Partial.
- [x] Portfolio, fills, trades, warnings, and eligible metrics come from
      persisted canonical records; no placeholder or recomputation is used.
- [x] Unsupported schemas and missing/corrupt result or data references produce
      structured visible errors without fallback.
- [x] Replay I/O is cancellable and asynchronous; stale work cannot update a
      newly selected result or destroyed view.
- [x] Step, step-back, seek, restart, speed, zoom, and result switching keep the
      bounded candle/volume/marker/portfolio/trade presentation synchronized.
- [x] Identical functional inputs produce identical canonical records/hash;
      independent runs retain distinct Result IDs.
- [x] Positive, negative, boundary, regression, contract/integration,
      sanitizer, thread-sanitizer, performance, and deterministic fixture
      evidence satisfies the repository Definition of Done.

## 7. Explicitly Deferred or Blocked

- Multi-symbol Replay UI, complete general broker/margin/metrics behavior,
  Python Strategy execution, and public market-data distribution are not
  implemented by issue #10.
- Unsupported capabilities remain explicit in the result schema and UI; no
  placeholder record implies implementation.
- Public data-bearing release remains blocked by redistribution and verified
  split-manifest requirements even when synthetic/local fixtures pass.

