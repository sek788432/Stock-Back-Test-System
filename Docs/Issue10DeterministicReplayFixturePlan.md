# Backtest Result K-Line Replay Implementation Plan

> **Current consolidation status (2026-10-13): Needs changes.**
> The maintainer now requests one consolidated implementation commit/PR before
> completing the remaining plan, superseding the earlier small-PR delivery
> preference. The preserved source is `6ac45ea`; merged main repairs from
> PRs #71/#74/#75 and the current delivery inventory must not be regressed.
> The integration checkpoint is not whole-plan acceptance or permission to
> merge known defects. Unused period values remain editable identity metadata,
> as delivered in #74; the source's period-disable behavior is excluded pending
> an explicit compatibility decision. Source-timeframe validation, live-writer
> ownership, pin-before-visibility, per-slice durability, historical conditions,
> the independent hash oracle and final UI acceptance remain open. Prior dated
> gates below are historical evidence, not verification of this integration.
> Follow the [delivery inventory](Plans/Issue10IncrementalDeliveryInventory.md)
> for consolidation coverage, exact-head gates and publication status.

> **Published checkpoint and approved repair (2026-10-13):** Draft
> [PR #76](https://github.com/sek788432/Stock-Back-Test-System/pull/76)
> publishes consolidated commit `d7ce3d1bd546cb8f5610ac54ab67c5a5951b0c75`.
> Its exact local full tests and Quality passed, but remote CI failed at
> macOS/Linux test compilation, Linux IWYU, and the long-range Replay coverage
> test. It is not merge-ready. PR #73 was closed as superseded only after its
> unchanged manifest-pin regression was verified in #76. The maintainer has
> now approved the R1–R3 SQLite lease / pin-before-visibility / atomic completed
> bar / committed-prefix recovery direction, including preservation and a
> warning for legacy unleased staging. Detailed design review, implementation,
> crash tests and acceptance remain pending; approval is not completion.

> **Acceptance status (2026-10-12): Needs changes, not fully complete.**
> The dated ledger preserves prior successes, failed gates, and open defects.
> The latest exact-HEAD full non-fast RunQuality pass was on `b5be0c8`
> (243/243 tests, 95.51 seconds, 98% changed lines, 94.0% changed branches).
> Its separate `./RunTest.sh` passed 243/243 in 84.54 seconds. This covers the
> intrinsic label-width, marker-clearance, Buy-only candle-grouping and R4
> full-money half-even repairs,
> not subsequent ledger edits or outstanding implementation requirements. The earlier
> `01620b9` full quality failure at IWYU is preserved below. Native Max playback reached
> 23,777/23,777 without the prior accumulating-left-margin failure, but this
> does not accept all layout and responsiveness requirements. Tasks 8, 9, 11A,
> 21, and 22 have accepted scoped checkpoints; Task 25's naming flow and the
> chart-density/Follow latest repairs have automated and isolated-app evidence,
> but are not final whole-plan acceptance. Results naming and Open Replay were
> exercised in the isolated macOS app. The approved test-only Trash/Restore
> cycle completed after restoring native window focus; the same AAPL ID/name
> returned to Saved Results beside the untouched Test 2 result.
> Tasks 10/11B/13–18/24 and the broader R1–R3/R6–R9 findings remain open,
> along with exact saved-data comparison, long-seek responsiveness, resize
> inspection, Task 26 marker clearance and progressive-layout acceptance,
> final review/gates, and delivered-checkout verification.
> The user authorized completing the work and creating a new PR after prior
> PR #70 merged; checkpoint commits are authorized.

> **Historical worker guidance:** The original plan required
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` for task-by-task work. Current execution follows
> the repository's canonical scope-sensitive playbook and consolidation ledger;
> dated checkboxes are not proof of current acceptance. Use inline execution
> unless delegation is approved; unresolved contract decisions remain explicit.

**Goal:** Persist every supported Backtest as a canonical `.bteresult` and let
the user open or select that result in K-line Replay to inspect its exact
hourly/daily candles within the saved Backtest range, volume, fills, and
portfolio history. Add readable buy ▲ / sell ▼ triangles and a larger chart;
prove the workflow using a real Selection Conditions Backtest with at least
five buys and five sells, not an imported hand-authored fill container.

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

## Integration Checkpoint Constraints

These describe the preserved implementation checkpoint, not an assertion that
the complete Save/draft lifecycle is accepted or already merged into main.

- Current worktree: every valid Backtest records a staged `.bteresult`, then
  finalizes a terminal draft outside the active Results catalog. **Save Result**
  publishes that draft; **Save & Replay** publishes the same ID and navigates.
  Draft listing/discard and interrupted-staging recovery are implemented in this
  limited slice. Final Task 14 acceptance remains pending.
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
- Legacy JSON summary storage was removed. It is never presented or converted
  as replayable Backtest Results; historical baseline wording below is not
  current implementation status.

---

## 1. Scope and Pre-Implementation Baseline

[Issue #10](https://github.com/sek788432/Stock-Back-Test-System/issues/10)
remains the final small, offline determinism fixture. The production
prerequisites below belong in separate linked issues/PRs; they must not be
hidden inside the issue #10 test change.

Verified behavior when this plan was written, before the implementation in
merged PR #68 and the follow-up hardening:

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
  persisted compatibility metadata and **is included** in schema-2
  `canonicalResultHash`; only the user's Hourly/Daily presentation choice is
  excluded. Do not change field inclusion/framing without a schema revision.

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

- Support the selected snapshot history, including the observed 23,777-hour
  AAPL result; 10,000 bars is an older minimum test size, not a history ceiling.
- Progressively present every selected candle and its volume through the
  current Replay cursor, without a fixed visible-frame truncation.
- Seek and step through indexed persisted records; avoid replaying financial
  execution from record zero. Rebuilding a long presentation prefix may need
  further optimization, but must not silently remove selected bars.
- Verify 1,000 catalog entries and document reproducible open, seek, maximum
  playback, and resident-memory measurements. Performance claims require
  measured evidence, not a hard-coded machine-independent latency promise.

## 3. Persisted Result Contract

The table below is the target general contract, **not a claim that schema 2
implements every field**. Current limited fields are defined by
[Engine/Results §8.1](Specs/EngineReplayPnL.md#81-implemented-schema-2-canonical-hash-framing)
and `Src/Backend/Results/Include/Bte/Results/ResultStore.h`.
Schema 2 does not yet represent complete Engine/API/source versions, runtime/
cost-profile identities, realized/unrealized/restricted-cash accounting or
general trade/margin/metric families. Current Replay presents candles, volume,
fills/rows, portfolio, Partial and terminal reasons, not complete warnings/
logs/capabilities/trade episodes/metrics. These omissions remain Planned, not
fabricated or silently accepted by narrowing this plan.

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
| Decisions/docs | Update the living `Docs/Decisions/ImportantDecisions.md` only for confirmed qualifying decisions; update descriptive owning specs, `Docs/Decisions/Dependencies.md`, and `Docs/KLineReplayRoadmap.md` |
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

## 5. Original Delivery Tasks — Existing Code, Acceptance Reopened

The original implementation history remains below. All original checkboxes are
reopened for current acceptance; this does **not** mean the code is absent.
Historical RED/mutation/commit-title claims are not independently proven merely
by inspecting today's tree. Current scope-limited passing evidence and specific
open gaps are tracked in §8. Check a step only with inspectable dated evidence.

### Task 1 — Freeze architecture, schema, and dependency decisions

**Files:** `Docs/Decisions/ImportantDecisions.md`, affected descriptive specs,
`Docs/Decisions/Dependencies.md`, `Docs/KLineReplayRoadmap.md`, root build files.

- [ ] Write the ADR covering Results module ownership, automatic lifecycle,
      Result ID/hash separation, canonical record framing, SQLite durability,
      recovery state machine, immutable-data retention, compatibility, and
      legacy isolation.
- [ ] Pin SQLite through the repository manifest, record exact version/license,
      and wire the planned Results target without relying on an untracked system
      installation.
- [ ] Correct delivery-status text where it claims a rendered volume pane or
      complete result behavior that the checkout does not contain.
- [ ] Link prerequisite issues and retain issue #10 as the final fixture only.
- [ ] Validate all changed links and status labels.
- [ ] Commit as `docs(results): define persisted replay architecture`.

**Exit:** reviewers can determine every persisted identity, record family,
ordering rule, lifecycle transition, compatibility error, and module dependency
without consulting implementation guesses.

### Task 2 — Build immutable snapshots and exact data selection

**Files:** Data public/private files, snapshot-builder target, Data tests and
fixtures, Data CMake registration.

- [ ] Add failing unit/contract tests for deterministic segment generation,
      manifest/hash validation, half-open selection, exact ordered row spans,
      UTC boundaries, missing/corrupt/out-of-order segments, and cancellation.
- [ ] Implement release-built content-addressed hourly segments and a read-only
      reader returning bars plus immutable Data Selection Identity.
- [ ] Add transactional reference acquisition/release, Trash restore, and purge
      race tests so a promoted result cannot lose required segments.
- [ ] Migrate Backtest data selection away from mutable path discovery before
      any result writer is enabled.
- [ ] Run `bte_data_tests` and the new contract tests; verify deliberate manifest
      mutation fails closed as `DataSnapshotUnavailable`.
- [ ] Commit as `feat(data): add immutable backtest data identity`.

**Exit:** the exact bars used before execution can be reopened later solely from
persisted identities and row spans.

### Task 3 — Add the transactional Results module

**Files:** new Results module, Results tests, root/module/test CMake files;
legacy `ResultSnapshotStore` was removed rather than retained as a competing
storage contract.

- [ ] Add failing tests for begin/append/finalize/promote, Result ID collision,
      canonical hashing, catalog ordering, schema/version mismatch, import,
      recovery, corrupt staging, Trash/restore/purge, and reference counts.
- [ ] Implement the deep Results interface: start one run, record ordered typed
      batches, finalize/promote, list summaries, open one result, and manage its
      lifecycle while hiding SQLite details.
- [ ] Reopen and validate before same-filesystem no-clobber promotion; incomplete
      artifacts never become catalog-visible.
- [ ] Fault-inject after schema creation, record transactions, hash finalization,
      close, promotion, and catalog visibility; verify restart recovery yields
      `Interrupted` only when trustworthy records exist.
- [ ] Prove canonical hash independence from save time, local path, SQLite page
      layout, and catalog order.
- [ ] Run the Results unit and contract test targets.
- [ ] Commit as `feat(results): add transactional backtest result store`.

**Exit:** an automatically recorded result survives restart, validates, lists,
opens, and retains its exact data without fabricated fields.

### Task 4 — Emit authoritative Engine records

**Files:** Engine Backtest interface/implementation, Results integration,
Engine tests.

- [ ] Add failing tests for post-slice portfolio checkpoints, multiple
      same-timestamp records, no-signal, one-bar, cancellation, operational
      failure, incomplete final mark, and deterministic repeat execution.
- [ ] Separate pre-run validation errors (no Backtest starts) from terminal run
      statuses that finalize diagnostic results.
- [ ] Emit fixed-point canonical orders, fills, portfolio/cost records, warnings,
      and stable sequence IDs through the Results recording seam.
- [ ] Ensure every processed hourly bar has one post-slice checkpoint after the
      accepted event sequence; Replay never derives accounting from fills.
- [ ] Compare Batch/Paced functional records and hashes for identical inputs.
- [ ] Run `bte_engine_tests` under normal and sanitizer presets.
- [ ] Commit as `feat(engine): record canonical backtest timelines`.

**Exit:** each supported Backtest automatically finishes with a promoted
completed or diagnostic Result ID and authoritative Replay records.

### Task 5 — Add asynchronous result-backed Bindings

**Files:** Backtest and Replay Bindings headers/implementations, Bindings tests
and CMake registration.

- [ ] Add failing tests for list/open/cancel, stale completion, corrupt entries,
      missing data, unsupported timeframe/policy, Hourly stepping, UTC Daily
      aggregation, immediate first-frame presentation, partial buckets, seek,
      and diagnostic truncation.
- [ ] Expose immutable result summaries and a result-backed Replay presentation
      model; keep backend fixed-point values authoritative until display
      conversion.
- [ ] Replace replay-from-zero rewind with indexed cursor/window access and
      precomputed daily bucket indices.
- [ ] Run store/data validation on owned cancellable workers; deliver only
      queued immutable values and ignore obsolete request generations.
- [ ] Remove schema fallback from both Backtest and Replay loading seams:
      execution accepts hourly only; presentation accepts Hourly/Daily only.
- [ ] Run the Bindings test targets, including rapid result switching and view
      destruction during open.
- [ ] Commit as `feat(bindings): expose persisted result replay`.

**Exit:** Bindings can open a Result ID and deterministically present bounded
Hourly/Daily frames without invoking Engine execution.

### Task 6 — Connect Backtest, Replay, and the application shell

**Files:** Backtest/Replay Frontend, chart interface/adapter, MainWindow, Qt and
shell tests.

- [ ] Add failing Qt tests for **Open in Replay** enablement, accessibility,
      shell navigation, exact ID selection, selector states, keyboard controls,
      immediate first-candle presentation, fills, volume, portfolio, trade
      rows, seek, and selection while playing.
- [ ] Add the Backtest footer CTA; it never saves and is enabled only after
      automatic result promotion.
- [ ] Make MainWindow own the narrow Result ID navigation request; tabs never
      find or manipulate each other.
- [ ] Replace primary symbol/date Replay setup with result selection. Keep any
      bar-only preview explicitly separated and remove placeholder accounting
      from result mode.
- [ ] Extend the chart interface once for bounded candle/volume/marker frames;
      render hourly source volume and checked Daily sums on the shared window.
- [ ] Render buy/sell markers with color plus shape, accessible fill details,
      synchronized trade rows, and **Partial** Daily labels.
- [ ] Verify no more than 500 bars render and maximum playback batches UI
      updates without blocking the event loop.
- [ ] Run all Qt tests offscreen and commit as
      `feat(replay): visualize persisted backtest results`.

**Exit:** users can open the just-finished result or select any available result
and inspect exact candles, volume, fills, and persisted portfolio state.

### Task 7 — Land issue #10 deterministic production-path fixture

**Files:** `Tests/Fixtures/`, end-to-end test source, `Tests/CMakeLists.txt`,
roadmap/issue traceability.

- [ ] Commit a small synthetic immutable hourly snapshot containing UTC-day
      boundaries, multiple fills, a partial bucket, and non-trivial volume.
- [ ] Build one literal canonical result through the production Results writer
      and implement `ReplayFixture_replaySnapshotsMatchExpectedSequence`
      through the real snapshot → `.bteresult` → catalog → Replay seams.
- [ ] Implement `ReplayFixture_replayTwiceProducesSameSnapshots`; assert
      identical literal presentation sequences from the same immutable result.
- [ ] Add a separate integration assertion that two identical Backtests produce
      distinct Result IDs and identical canonical records/hashes; do not present
      that assertion as issue #10's original test-only acceptance criterion.
- [ ] Assert exact candles, volume, stable fill order, post-slice portfolio,
      progress, Daily aggregation, and absence of Engine/Strategy calls during
      Replay.
- [ ] Deliberately mutate one production behavior and confirm the focused test
      fails before restoring the source.
- [ ] Run the focused target, complete CTest presets, `./RunTest.sh`, and
      `./RunQuality.sh --base <base-revision> --head HEAD` on the exact committed
      revision.
- [ ] Commit as `test(replay): add deterministic K-line replay fixture`.

**Exit:** issue #10 proves stable behavior through the actual persisted-result
workflow, offline and independent of wall-clock time.

## 6. Whole-Plan Acceptance Checklist — Not Currently Accepted

- [ ] Every valid supported Backtest automatically produces one validated
      `.bteresult`; crash recovery never labels an incomplete file Completed.
- [ ] **Open in Replay** opens the exact promoted Result ID without saving again.
- [ ] Opening a non-empty result immediately shows its first Hourly candle or
      Daily bucket with synchronized volume, fills, and post-slice portfolio;
      the next Step advances exactly once.
- [ ] Replay can select completed and diagnostic results and isolates unavailable
      catalog entries.
- [ ] Referenced hourly candle records are identical to the canonically
      normalized immutable input selected before execution.
- [ ] Daily Replay follows `utcCalendarDayV1`, sums volume, includes every fill,
      uses the last post-slice checkpoint, and labels truncated buckets Partial.
- [ ] Portfolio, fills, trades, warnings, and eligible metrics come from
      persisted canonical records where implemented; unsupported families stay
      explicitly Planned. Complete warning/capability presentation is not yet accepted.
- [ ] Unsupported schemas and missing/corrupt result or data references produce
      structured visible errors without fallback.
- [ ] Replay I/O is cancellable and asynchronous; stale work cannot update a
      newly selected result or destroyed view.
- [ ] Step, step-back, seek, restart, speed, zoom, and result switching keep the
      bounded candle/volume/marker/portfolio/trade presentation synchronized.
- [ ] Identical functional inputs produce identical canonical records/hash;
      independent runs retain distinct Result IDs.
- [ ] Positive, negative, boundary, regression, contract/integration,
      sanitizer, thread-sanitizer, performance, and deterministic fixture
      evidence satisfies the repository Definition of Done.

## 7. Explicitly Deferred or Blocked

- Multi-symbol Replay UI, complete general broker/margin/metrics behavior,
  Python Strategy execution, and public market-data distribution are not
  implemented by issue #10.
- Unsupported record families must remain explicit and never fabricated.
  Complete capability/warning UI is not implemented; do not imply that a backend
  family or metadata declaration proves its presentation.
- Public data-bearing release remains blocked by redistribution and verified
  split-manifest requirements even when synthetic/local fixtures pass.


## 8. Honest Tracking and Confirmed Follow-up Plan

### Tracking rules and fixed evidence point

Historical review point: `.worktrees/issue10`, branch
`codex/finish-issue10-plan`, HEAD `cb58dd9e5e238bb5be7b3804946687482c0b1956`,
merge-base `e3de43bba676a8ce63953b04a49d859df8897b6e`.
Current 2026-10-09 worktree: `.worktrees/issue10`, branch
`codex/issue10-replay-ux`, HEAD `4dfcbd35c976b1fa027a1d932b4023c85f71a138`,
merge-base `84e3a135b18bdddb8134713df0b6896be0cc1a37`, plus uncommitted
chart, test and documentation edits. This is not a final submission revision.
[PR #70](https://github.com/sek788432/Stock-Back-Test-System/pull/70) follows
merged PR #68. Some defects predate #70 but directly affect whole-plan acceptance.

| State | Meaning |
| --- | --- |
| Verified baseline | Observed at the pinned HEAD for the named cases only |
| Implemented / not accepted | Code exists; complete/new behavior is not proven |
| Open defect | Reproduced failure, no fix verified |
| Planned | Not started; design approval is not implementation |
| Blocked | Named prerequisite prevents acceptance or release |
| Accepted | Implementation, regressions, applicable gates, docs and review pass |

- `[x]` requires dated revision, exact command/UI scenario, actual outcome,
  review decision and limitations. `[ ]` means not accepted, not necessarily absent.
- Record RED or Added coverage, implementation, GREEN, full gates and desktop/
  review separately. Never fabricate a RED failure for a newly added passing test.
- Preserve previous successes and failures. CI green does not negate reproduced
  errors. Report Failed, Blocked, Not run and N/A with precise reasons.
- Do not claim filtered tests are the full suite, sanitizer coverage of unbuilt
  Qt tests, current evidence from old CI, or commit/mutation steps not verified.
- Update this ledger after each confirmed task. Scope changes/deferrals require
  explicit confirmation. Commit/push/new PR authority was granted on 2026-09-16
  after #70 merged, conditional on completing the scoped work and required gates.
  No new dependency or unrelated repair scope is approved.
- Tasks 8–11 are confirmed for implementation (2026-09-16); agree a separate
  repair design before expanding into R1–R4/R6/R7 fixes. R5 belongs to Task 8.
  The bounded R4 full-integer half-even repair was separately confirmed on
  2026-10-12; other unresolved repair-design gates remain unchanged.
  Whole-plan acceptance cannot pass while applicable defects remain open.

### Existing implementation versus remaining work

| Original task | Existing work verified in review | Current limitation |
| --- | --- | --- |
| 1 Architecture/schema | Results ownership, schema 2, pinned SQLite and living decision exist | Earlier hash/metadata/presentation completion claims were inaccurate; this edit corrects plan wording only |
| 2 Immutable data | Real synthetic snapshots, selected spans and validation tests | R2 retention/promotion consistency remains open; public release Blocked |
| 3 Results lifecycle | Writer/catalog/import/recovery/Trash and fault tests exist | R1/R2; manual writer recovery is not real Engine mid-run recovery |
| 4 Engine records | Real orders/fills/portfolio and Batch/Paced/repeat tests | R3 execution-time persistence absent; R4 scoped parity repair accepted at b5be0c8 |
| 5 Async Bindings | Indexed Hourly/Daily, cancellation/stale rejection, diagnostic cutoff | R5 boundary repaired at `a098577`; full warnings/capability UI still absent |
| 6 Desktop workflow | Normal Backtest→exact ID→Replay; volume, log, navigation/zoom | Buy triangle but sell rectangle; chart minimum height 300px; new size and multi-fill condition acceptance not done |
| 7 Offline fixture | Production Snapshot→Writer→Catalog→Replay, separate Engine determinism test | Literal fixture is not Selection Conditions; R6 golden/same-time/Daily-repeat gaps |

### Baseline review findings — current repair evidence tracked below

| ID | Evidence / severity | Required correction and regression |
| --- | --- | --- |
| R1 | P1: second ResultStore open quarantined a live empty writer; original finalize failed | Ownership/lease must distinguish active writers from abandoned staging; concurrent catalog/open/recovery tests |
| R2 | P1: retention DB failure returned import error but promoted result remained visible; recovery has analogous ordering | Recoverable pin/promotion/visibility protocol; rollback/restart and retention fault integration |
| R3 | P1: one Engine slice processed, 3 memory records but 0 staging records; restart yielded Quarantine, no Interrupted catalog entry | Atomic execution-time slice/batch persistence; real mid-run interruption/restart and Batch/Paced tests |
| R4 | P2: 1 share at $1.0000015 marked 1000001 microdollars, expected half-even 1000002 | Full monetary integer parity; odd/even ties and adjacent boundaries |
| R5 | P2: first trusted checkpoint at same-day 01:00; Hourly open/volume 103/3400, Daily 101/5700 | Exclude pre-checkpoint bars consistently; same-day diagnostic/UTC range tests |
| R6 | P2: same-implementation hash comparisons lack literal schema-2 framing/hash oracle | Golden hash/bytes, absent-v-zero, ordering, same-time fills and Daily repeat |
| R7 | P2: metadata and warning/trade/metric completion exceeded schema/public Replay support | Honest schema limits and supported warning/capability presentation or explicitly confirmed deferral |

R1/R2 production location: `Src/Backend/Results/Private/ResultStore.cpp`;
R3/R4: `Src/Backend/Engine/Private/Backtest.cpp`;
R5: `Src/Bindings/Replay/ResultReplay.cpp`.
Review probes are not yet checked-in regressions. Temporary evidence can expire;
reproduce and add permanent tests before claiming a fix.

### Confirmed new acceptance contract

- Result mode uses saved UTC `[start,end)` and exact selected spans, not current
  editable preview dates or bars outside the saved selection. The complete
  selected K-line timeline is loaded and indexed, but presentation starts with
  the first K-line only. Step/Back/seek/play/speed grow or shrink the rendered
  candle prefix together with the replay cursor and portfolio. Fills appear only
  when their actual persisted bar appears; future candles and fills remain hidden.
- Daily OHLCV uses only selected trusted bars; full-source comparison may label
  Partial but may never add out-of-range/pre-checkpoint bars to the aggregate.
  Hourly and Daily cover the same saved `[start,end)` range and remain freely
  switchable; only the presentation granularity and cursor mapping differ.
- Buy is green upward ▲, sell red downward ▼, distinguishable by orientation
  without color. Mark actual persisted fill bar/time/price, not signal/order time.
- Coincident fills remain individually identifiable with deterministic visual
  offsets and exact accessible details. Stored coordinates/trade rows never change.
  Daily retains every fill; rewind/seek/switch cannot leave duplicate/stale icons.
- Chart-first layout: compact result/range/timeframe/playback header, collapsible
  settings/explicitly separate legacy preview, compact portfolio and an adjustable
  keyboard-accessible chart/trade-log splitter.
- Proposed measurable sizes: at 1200×800 and 1440×900 logical pixels, default
  chart panel occupies at least 55% of available Replay content height. Inspect
  1024×768 reflow: no horizontal scroll or unreachable/clipped controls; permit
  vertical scrolling. Check collapse/expand, splitter limits, focus and resize.
  These thresholds were confirmed on 2026-09-16; no old screenshot is said to pass.
- Replace the current sliding 500-frame presentation with an untruncated
  cursor-bounded timeline. At cursor `N`, candles `0..N` are presented and
  remain navigable; candles after `N` are not presented until playback reaches
  them. Rendering may virtualize or level-of-detail long histories, but it must
  not discard already reached candles when the cursor passes 500. At the final
  cursor the complete selected range is visible. No general editor, multi-symbol,
  broker/margin/metrics or chart dependency replacement is silently bundled.
- Real Selection Conditions (UI label: Selectable Conditions) must generate
  ≥5 buys and ≥5 sells through actual Engine→saved Result→desktop Replay.
  Existing literal/imported result tests remain valuable but are not substitutes.
  Current runtime supports two conditions per group, not the planned five-row editor.

### Task 8 — Exact-range and trusted-prefix Replay

**Status:** Backend trust boundaries accepted for Task 8 / R5 at `a098577`;
the 500-frame frontend presentation limit was removed by accepted Task 21.
**Modify:** `Tests/Unit/Bindings/UnitTest_ResultReplay.cpp`,
`Tests/Unit/Replay/UnitTest_ReplayFixture.cpp`,
`Src/Bindings/Replay/ResultReplay.cpp`, `Docs/Specs/EngineReplayPnL.md`.
**Interfaces:** existing saved descriptor/spans/records → ResultReplay frames;
no Engine call, public API or hash-framing change.

- [x] RED: selected bars before/at start and before/at end prove half-open
  selection. Same-day first-checkpoint regression expects Daily open 103 and
  volume 3400 rather than 101/5700. Cover diagnostics, UTC Partial, empty/error,
  no future fills and checked volume overflow.
- [x] Build `cmake --build --preset qt-dev --parallel`; run the command below;
  save actual failures before production changes.
- [x] Correct aggregation/selection trust boundaries minimally, retaining indices,
  cancellation and fixed-point validation.
- [x] GREEN: repeat command and verify Hourly/Daily repeat, step/back/seek;
  update spec and ledger, request review before accepting R5.

```bash
ctest --test-dir Output/qt-dev -R 'ResultReplayTest\.|ReplayFixture\.' --output-on-failure
```

### Task 9 — Opposite, readable fill triangles

**Status:** Accepted at `e43c7fb` for marker presentation and zoom behavior;
final whole-branch independent review remains part of Task 12.
**Modify:** `Src/Frontend/Charts/QtChartsCandlestickView.cpp`,
`Tests/Unit/Frontend/UnitTest_QtChartsCandlestickView.cpp`,
`Tests/Unit/Frontend/UnitTest_ResultReplayTab.cpp`, `Docs/Specs/FrontendQt.md`.
**Interfaces:** existing `setMarkers` span → chart glyphs/exact fill descriptions;
stored fill values and synchronized rows are unchanged.

- [x] RED: check geometry/orientation, not only count/color: buy ▲, sell ▼,
  none/multiple/coincident fills, stale clearing, seek/window boundaries and
  grayscale distinction. Capture original sell-rectangle failure.
- [x] Build and run command below, then implement opposite triangle orientations
  and deterministic overlap presentation behind the existing chart seam.
- [x] GREEN: repeat command; inspect ten Engine-generated fills at normal size,
  zoom and Daily; no missing/clipped/duplicate icon. Update docs/ledger and review.

```bash
ctest --test-dir Output/qt-dev -R 'bte_frontend_chart_tests|bte_result_replay_frontend_tests' --output-on-failure
```

### Task 10 — Larger adjustable chart-first layout

**Status:** Implementation and automated layout checks are green; actual desktop
acceptance remains pending. The earlier locked-session obstacle is historical;
the Mac is now available, but three-size, splitter and while-playing interaction
acceptance has not yet been completed. The default
chart-first state keeps Replay setup collapsed behind an English-labelled
keyboard-focusable toggle; playback and portfolio remain visible. A vertical
chart/trade-log splitter owns the remaining space, preserves a readable chart
minimum and permits the trade log to expand.
**Modify:** `Src/Frontend/Replay/ReplayTab.cpp`, `ReplayTabSections.h/.cpp`
and `ReplayTabStyle.h/.cpp` in that directory as needed;
`Tests/Unit/Frontend/UnitTest_ResultReplayTab.cpp`, `Docs/Specs/FrontendQt.md`.
**Interfaces:** existing setup/playback/chart/portfolio/log sections → collapsible,
resizable display, without accounting changes.

- [x] RED: Qt geometry tests at both target sizes expect ≥55% chart share;
  test 1024×768 reflow, collapse/expand, splitter limits, keyboard traversal,
  preview/result separation and disabled/error states.
- [x] Build/run command below; record actual old-layout assertions before changes.
- [x] Implement compact settings and splitter; keep controls/summary readable,
  avoid horizontal clipping and preserve the full selected timeline contract.
- [ ] GREEN: repeat tests; operate fresh app at three sizes, resize while playing,
  collapse/expand and drag splitter. Record before/after dimensions and visual
  observations/screenshots, actual platform and limitations; update docs/review.

```bash
ctest --test-dir Output/qt-dev -R 'bte_result_replay_frontend_tests|bte_app_shell_tests' --output-on-failure
```

### Task 11 — Actual multi-fill Selection Conditions Backtest→Replay

**Status:** 11A offline generation/contract coverage accepted at `6b9b3e7`; 11B desktop
full date-selection/save/Replay acceptance not started. Task 9 already includes
a real 5-buy/5-sell desktop run; that narrower evidence does not complete 11B.
Task 11 as a whole is not accepted.
**Modify:** `Tests/Unit/Replay/UnitTest_ReplayBacktestDeterminism.cpp`,
`Tests/Unit/Frontend/UnitTest_BacktestTab.cpp`,
`Tests/Unit/Frontend/UnitTest_ResultReplayTab.cpp`, `Tests/CMakeLists.txt`
only if registration changes. Keep `UnitTest_ReplayFixture.cpp` in the same
Replay test directory Engine/Strategy-independent.
**Create:** `Tests/Fixtures/ReplayMultiFill/SYN.csv` — synthetic test inputs only.
**Interfaces:** real `SelectableStrategyPlan` → Backtest → promoted Result ID
→ existing ResultReplay; no imported literal fill container substitutes for Engine.

**Dependency correction:** Task 9 requires ten actual Engine fills before its
GREEN visual check. Execute 11A (offline real Engine generation and literal
contract checks) → 9 → 10 → 11B (fresh desktop scenario). Each slice must pass
its full applicable gates before the next begins. This preserves, rather than
defers, Task 9's input requirement; 11A alone does not complete Task 11.

- [x] Build a synthetic immutable hourly snapshot at UTC 2024-01-02 00:00–13:00
  inclusive (14 bars). Opens 100, highs 102, lows 98, volume 1000; closes
  alternate 99/101 beginning 99. Select `[01:00,13:00)` (12 bars), leaving
  before-start/end sentinels to prove exact range.
- [x] Configure buy close <100, sell close >100, ALL, one condition per group,
  10 shares, capital 10000. Actual next-bar buys must occur at 03/05/07/09/11,
  sells at 04/06/08/10/12: exactly 5 buys/5 sells, final position zero.
  Flat sell at 01:00 is not a fill. Assert literal timestamps, sides, quantities,
  prices/costs under the current fixed slippage policy and every portfolio slice.
- [x] Boundary variant end=12:00 excludes fifth sell: 5 buys/4 sells, open position.
  Never misreport it as five completed round-trips. Add no-signal and invalid
  condition/no staged-result side effects, pending final order, same Daily bucket.
- [ ] Add contract checks for selected candles, actual fills/triangles, saved
  portfolio and ID navigation; build and run command below. If added assertions
  already pass, record Added coverage, not fabricated RED. Repeat Backtest:
  distinct IDs/equal canonical records/hash. Repeat Replay in both timeframes.
- [ ] Desktop date-only variant: build an isolated snapshot with one sentinel
  at 2024-01-01 23:00, eleven bars at 2024-01-02 00:00–10:00, and an end
  sentinel at 2024-01-03 00:00. Use the same OHLCV alternation and conditions.
  Select Start=End=2024-01-02 in the current date-only UI, which Bindings maps
  to UTC `[2024-01-02T00:00,2024-01-03T00:00)`. Expected buys are
  01/03/05/07/09 and sells 02/04/06/08/10: exactly 5/5, final position zero.
  Do not pretend current app can select the integration test's 01:00 boundary.
- [ ] In isolated freshly built app select **Selectable Conditions**, set the
  date-only variant with capital 10000 and quantity 10, Run actual Engine,
  inspect 5/5 trade rows, then exact-ID CTA.
  Verify triangles/portfolio through Step/Back/seek/play/Hourly/Daily/result
  switching, including selection while playing. Starter/imported fixture is not
  sufficient. Record actual parameters, counts, IDs, snapshot, outcomes and gaps.

```bash
cmake --build --preset qt-dev --parallel
ctest --test-dir Output/qt-dev -R 'ReplayBacktestDeterminism\.|bte_backtest_frontend_tests|bte_result_replay_frontend_tests' --output-on-failure
```

### Task 12 — Full re-acceptance and honest handoff

**Status:** Planned. **Files:** this plan, affected owning specs,
`Docs/KLineReplayRoadmap.md` when its claims change; permanent tests for each
confirmed repair. **Interfaces:** task evidence/R-item disposition → current
acceptance verdict, not a universal bug-free promise.

- [ ] Agree repair designs/scope for R1–R7, implement confirmed corrections with
  regressions or obtain explicit scoped deferral; keep whole-plan acceptance
  Needs changes while applicable correctness/compatibility defects remain open.
- [ ] Review code/docs/test coverage, every effective committed/staged/unstaged/
  untracked change; run full tests, sanitizers, performance and complete quality
  commands below on the actual new implementation revision.
- [ ] Before requesting CI, obey DoD: `./RunTest.sh` then full
  `./RunQuality.sh --base <base-revision> --head HEAD` pass on exact committed
  HEAD. `--fast` is not full acceptance. Record failures/retries/warnings honestly.
- [ ] Update ledger after every task; hand off Accepted / Needs changes / Blocked
  with remaining work. New metadata/hash/schema or retention policy decisions
  require owning-spec/important-decision review before changes.
- [ ] Include Tasks 13–20 in final scope disposition. Each new requirement must
  be accepted with evidence or explicitly scoped/deferred by the maintainer;
  merely recording it here does not approve its unresolved semantics.

```bash
./RunTest.sh
cmake --preset dev-sanitize
cmake --build --preset dev-sanitize --parallel
ctest --preset dev-sanitize --no-tests=error
cmake --preset dev-tsan
cmake --build --preset dev-tsan --parallel
ctest --preset dev-tsan --no-tests=error
./RunQuality.sh --base 84e3a135b18bdddb8134713df0b6896be0cc1a37 --head HEAD
```

### Dated baseline evidence — does not verify future changes

| Check/scenario at pinned HEAD, 2026-09-16 | Actual result / limit |
| --- | --- |
| Normal isolated 5-bar Backtest→exact-ID CTA | Verified baseline; no extra save; final accounting matches Replay |
| First frame, Step/Back, foreground Right, seek, zoom | Verified baseline in named small scenario, not multi-fill Conditions |
| Literal Daily buy/sell, Partial, failed prefix/reason, missing-ID clear | Verified baseline; imported fixture is not real condition-run acceptance |
| `./RunTest.sh` | Passed 211/211, 58.40s; third-party/linker warnings visible |
| ASan/UBSan | Passed 180/180, 54.45s; Qt Replay/frontend tests not built |
| TSan | Passed 180/180, 108.35s; same Qt coverage limit |
| Full `RunQuality.sh` first invocation | Exit 1: static checks completed; coverage GoogleTest clone failed sandbox DNS; not a full exit-zero invocation |
| Coverage resumed using approved network access | Configure/build/211 tests passed, 57.30s; gcovr and both coverage gates passed |
| Changed coverage | Line 306/309 (displayed 99%); branch 22/23 (95.7%); reproduced bugs still exist |
| 10,000 frames | Open 284ms, seek 0µs, model traversal 90µs, peak 52672KiB; not full UI rendering throughput |
| 1000-entry catalog | 82ms, 1 valid +999 unavailable; 1000 valid containers unmeasured |
| R1–R4 production probes | Failures reproduced; diagnostic exit zero was not acceptance |
| R5 isolated same-day test | Exit 1, two failing assertions; not yet permanent repository regression |
| Historical GitHub checks | Reported successful at review time; no fresh CI evidence for this document edit/future revision |
| This plan edit | Docs-only; build/runtime implementation checks N/A; confirmation pending |

The following checkpoint ledger retains the evidence recorded at each named
revision, rather than presenting every State cell as current acceptance. Later
dated task records and the current consolidated checklist govern remaining work.
In particular, Tasks 10, 15 and 16 below retain historical desktop/implementation
limitations that were subsequently changed; Task 11B's full scenario remains open.

| Task/repair | State at named checkpoint | RED / coverage | GREEN | Full gates/revision | Desktop/review at checkpoint | Remaining at checkpoint |
| --- | --- | --- | --- | --- | --- | --- |
| 8 / R5 | Backend trust boundaries accepted; frontend presentation completed by Task 21 | Same-day OHLCV: 6 failures across Completed/Failed; pre-trust Daily fill: 1 failure. Later diagnostic end/range assertions: Added coverage, not RED | Final focus 24/24, 4.15s | `a098577`: RunTest 214/214, 55.54s; full non-fast RunQuality exit 0, coverage suite 214/214, 58.60s; changed lines 6/6, branches 2/2. ASan 180/180 and TSan 180/180 exclude Qt | Three independent review axes completed; test-axis P2 resolved with Daily future-boundary coverage. New-code desktop not run; Task 8 does not change widgets | None for Task 8; broader plan repairs remain separately tracked |
| 9 | Accepted at e43c7fb | Original sell marker was a rectangle; geometry/orientation, overlap, stale clearing and zoom-viewport tests now cover the behavior. The viewport test first failed because range notifications rearranged glyphs before Qt applied the new transform | Focused chart + result-Replay tests 2/2; final chart suite includes the zoom regression | Exact checkpoint RunTest 220/220, 57.62s before the zoom repair and 220/220, 54.59s after it; final non-fast RunQuality exit 0, coverage suite 220/220, 61.18s; changed lines 113/113, branches 16/16 | Isolated macOS desktop: actual Selectable Conditions Engine run produced 5 buys/5 sells; Hourly showed five green ▲ and five red ▼, Daily retained ten separated triangles and exact ten trade rows. Root self-review found no remaining Task-9 P0–P2; final independent whole-branch review is deferred to Task 12 | Broader layout and Task 11B workflow remain open; Qt is excluded from sanitizer presets |
| 10 | Automated implementation green at `8256300`; desktop acceptance blocked by locked macOS session | New chart-first test first failed because `replaySetupToggleButton` did not exist. The first splitter implementation then exposed a real headless Qt Charts construction crash; a fixed chart-panel size contract removed the screen-dependent `QGraphicsView::sizeHint` call | Focused geometry test passes offscreen at 1024×768, 1200×800 and 1440×900; setup toggle, splitter limits and zero collapsed horizontal overflow are asserted. Replay/frontend/app-shell regression 3/3, 14.37s; RunTest 220/220, 61.02s | First non-fast quality attempt failed IWYU; second passed static analysis but failed sandbox DNS during coverage setup; approved-network rerun passed 220/220 in 62.12s, changed lines 248/248 and branches 41/44 | Offscreen 1200×760 snapshot inspected: collapsed setup, large chart, visible playback and adjustable details. Actual app was launched with the 520-Hourly/22-Daily fixture stores, but CUA reported the Mac locked; no desktop size/drag/while-playing claim | Unlock and perform three-size desktop interaction; then review exact revision |
| 11 | 11A accepted at 6b9b3e7; 11B planned | Added coverage — no Engine behavior change or fabricated RED | 7/7 offline integration tests; literal fill/cost/12 portfolio slices, range boundary, pending order, invalid/no-signal and repeat replay | Exact checkpoint RunTest 220/220; full non-fast Quality retry exit 0; ASan/TSan 180/180 exclude Qt | Three independent review axes completed; desktop not run | Actual 5/5 Conditions desktop run, triangles and layout still pending |
| R1/R2/R3/R6/R7 | Open | Review evidence above | No fix verified | No repair revision | Repair design pending | Must remain tracked |
| R4 | Scoped repair accepted at b5be0c8 | Public literal odd-base ties failed under the old component-only parity | 11 worked cases plus full-sum int64 boundary; Engine52/52 | Exact b5be0c8 RunTest243/24384.54s; full non-fast Quality exit0, coverage243/24395.51s, lines2067/2106, branches235/250; precommitASan/UBSan200/20051.76s and TSan200/20080.21s | Three independent review axes found no blocker; no widget behavior change | R6 literal hash oracle and whole-plan acceptance remain separately open |
| 12 | Planned | N/A — aggregation | Not accepted | Not run for future revision | Not run | Full DoD and honest verdict |
| 13 / R8 | Reported / not reproduced | No regression yet | Not run | Not run | Screenshot captures AAPL 2018-05-01–2024-05-16, Backtest Bars 23777; Replay count/ID unknown | Long range / sparse candles diagnosis and proven correction |
| 14 | Implemented on branch; final acceptance pending | RED for absent draft APIs/buttons and prior automatic publication; review found and RED-tested multi-draft selection, live-writer recovery, cancellation-after-open and discard-removal rollback boundaries | Exact `15dfcab` RunTest 232/232 | `d0fc1c8` failed changed-line coverage at 93%, `27a1381` failed at 96%; exact `15dfcab` full non-fast quality passed static analysis, scan-build and 232/232 coverage tests, changed lines 832/845 = 98%, changed branches 103/108 = 95.4% | Actual AAPL 48-bar Save Result published ID `4c37a35f…96a6` without navigation; Save & Replay opened the same ID. A later saved ID `649b0cd43bf43c96f4d674d428b1c7bd` opened at 1/48, stepped to 11/48 with varied OHLC and its Buy on the containing bar; Daily showed 1/3 after switching | Final all-path review, multi-fill and Results workflow remain; cross-process writer recovery is unverified |
| 15 | Partial UI implemented through `12958ea`; not accepted | RED for absent Trash list, async adapter, Results page and shell navigation; tests cover real snapshot-backed result, same-ID details/Replay, cancel/confirm Trash, restore/hash, corrupt entry, unavailable store, diagnostic statuses, stale request and currently open Replay invalidation | `12958ea` full quality coverage suite 236/236; earlier working-tree `RunTest.sh` 236/236 | Exact `12958ea` full non-fast RunQuality exit 0; changed lines 1230/1252 = 98%, branches 154/164 = 93.9%. Earlier `d469473` failed 95% changed-line coverage, and intervening IWYU/module-boundary failures were corrected | New Results page has not been desktop-operated: CUA reported Mac locked after the isolated app was launched with `/private/tmp/bte-results-desktop.tHUlMl/Results` | Multi-entry ordering, more stale/error/keyboard/size scenarios, actual Save→Results→Replay→Trash/Restore, 30-day policy, review and whole-plan acceptance remain |
| 16 | Planned / not implemented | Backend domain mapping exists; UI unit acceptance not run | Not run | Not run | Not run | Units, scale and examples for every offered metric |
| 17 | CSV/UTC data-day convention approved / not implemented | Prior-bar percentage exists; previous-data-day condition does not | Not run | Not run | Not run | Existing immutable CSV-derived source; previous available UTC date's last close |
| 18 | Partially clarified / period-anchor decision pending | Not run | Not run | Not run | Not run | Period gain (for example 30 days), historical-high drawdown and monthly high/low breakout; calendar/trading-day anchor pending |
| 19 | Worktree launch path and exact implementation gates accepted at `c28ddec`; final delivered-checkout acceptance pending | Launcher behavior has positive, partial/invalid configuration, missing manifest and builder-failure tests | Project script suite 19/19; Launch.sh built the exact worktree and started with snapshot `229c37d2…74acf`; RunTest 221/221, 62.57s | Exact implementation revision `c28ddec`: non-fast RunQuality exit 0; coverage suite 221/221, 64.42s; changed lines 279/279, branches 49/52; clang-tidy, cppcheck, IWYU and scan-build passed | Same built executable was operated through its macOS bundle identity; AAPL Backtest completed and Open Replay worked. Raw unbundled process could not be bound by the desktop automation API, so literal raw-process interaction is not claimed | Recheck from the final delivered checkout and record its revision/process identity |
| 20 / R9 | Launcher/snapshot implementation and actual starter workflow accepted at `c28ddec`; overall task remains partial | Launch tests first failed because no builder/configuration existed. Canonical seven-column snapshot test then failed with `Snapshot source has unsupported columns` | Data suite 37/37; project scripts 19/19. Missing/partial/invalid configuration and builder failure stop before app; tracked seven-column CSV builds successfully; RunTest 221/221, 62.57s | Exact implementation revision `c28ddec`: full non-fast RunQuality exit 0; coverage suite 221/221, 64.42s; changed lines 279/279, branches 49/52. Earlier attempts failed because `VCPKG_ROOT` was absent, then because `readSourceBars` exceeded the cognitive-complexity limit; the helper extraction corrected the latter without suppression | Launch-created snapshot `229c37d2…74acf`; actual AAPL starter Backtest completed 48 bars, result `0bbe3c12…a1456`; Open Replay progressed 1/48→12/48 with varied OHLC and one Buy marker | Later multi-fill condition-run Save/Results workflow; developer snapshot is not release data |
| 21 / R10 | Accepted at `05c23eb` after correcting the all-candles-first desktop rejection | New progressive-prefix expectations produced 5 frontend failures: initial small/Hourly/Daily counts were 3/520/22 instead of 1; timeframe switch was 520 instead of 49; multi-fill initial was 520 instead of 1. Regression covers 499→500→519 as 500→501→520 candles | Focused Result Replay + chart suites 2/2, 9.64s; RunTest 220/220, 61.73s. Initial 1, Step 2, seek 12 gives 13, rewind gives 1, max batch gives 34; markers/log follow the same prefix | `05c23eb`: full non-fast RunQuality exit 0; coverage suite 220/220, 3608.61s; changed lines 199/199, branches 41/44; clang-tidy, cppcheck, IWYU and scan-build passed. Earlier attempts honestly failed clang-format, missing `VCPKG_ROOT`, then an ownership-transfer clang-tidy diagnostic | Actual app result `fdf8b59ca7681dfc575a744a5ae40ce9`: 520 Hourly, 22 Daily, 149 fills. Hourly 1/520→2/520→13/520 showed 0→1→4 fills; Play advanced to 7/520 and Pause stopped; rewind returned 1/520. Daily mapped to 1/22, Step 2/22, Back 1/22 with fills anchored to their day. Normal-size screenshots were inspected in the desktop session but not saved as repository artifacts | Broader layout and other plan tasks remain open; 23,777-candle terminal batch is untruncated but measured 3.606s, so no claim of instant rendering |
| 22 / R11 | Accepted at `6f6fc86` | New pixel-geometry test failed: Buy center y=180.97 while its candle low was y=273.75, proving the glyph used fill-price height instead of candle-relative placement | Focused chart + Result Replay suites 2/2, 9.55s. Daily differing fill/candle timestamps and existing overlap/zoom/prefix cases are covered; RunTest 220/220, 62.14s | Exact implementation revision `6f6fc86`: approved-network non-fast RunQuality exit 0; coverage suite 220/220, 62.84s; changed lines 260/260, branches 41/44; clang-tidy, cppcheck, IWYU and scan-build passed. Earlier attempts honestly failed the custom CPP013 audit on a structured binding, then sandbox DNS during coverage setup | Actual app result `783ed8ed88b0018fbb1df227b92480b2`: 520 Hourly bars, 22 Daily buckets and 149 fills. Hourly at the 108-bar prefix placed every visible Buy below its candle low and every Sell above its high. Daily progressed 1/22→2/22 by Step and played to 11/22 before Pause; multiple same-candle glyphs remained separated and the log stayed synchronized. No repository screenshot was saved. The source was a cyclic synthetic stress fixture (`open = 100 + index % 7`), so its repeated candle shapes are not evidence of real-market visual fidelity | Marker placement is accepted. Future visual acceptance must use real varied OHLC; the cyclic fixture proves playback/count/overlap only |

Unknown/unverified stays explicit: every platform's desktop UX, full release
packaging, 1000-valid-result catalog performance, Qt-path sanitizer coverage,
and universal absence of bugs. Synthetic tests cannot clear public data blockers.

**Confirmation gate:** Tasks 8–11 were confirmed on 2026-09-16. Task 8 has a
reviewed green checkpoint; 11A has an accepted offline checkpoint; Task 9 has
an exact-commit implementation and desktop checkpoint on 2026-09-22. No larger
layout, Task 11B completion, push, new PR or fresh CI is claimed. The maintainer
confirmed PR #70 merged and authorized a new PR after completion; current branch
is `codex/issue10-replay-ux` based on `84e3a135`. R1–R4/R6/R7 repair designs
still require confirmation. Full checks must be repeated on each submission HEAD.

The later 2026-10-12 maintainer approval authorizes the bounded R4 correction;
this does not approve the other repair designs or claim R4 implementation passed.

### Implementation slice ledger (2026-09-16)

Historical review revisions above remain unchanged. New-PR fixed base is merged
main `84e3a135b18bdddb8134713df0b6896be0cc1a37` (#70), whose tree exactly matches
the old reviewed HEAD. Current branch is `codex/issue10-replay-ux` in the same
isolated worktree. The user authorized finishing then creating a new PR;
checkpoint commits are now authorized. Prior evidence is still explicitly
working-tree evidence until rerun on committed checkpoints.

| Order | Behavior / seam / scope | Fast evidence | Acceptance gate / blocker |
| --- | --- | --- | --- |
| 8 | Exact selected trusted Hourly/Daily bars; existing ResultReplay API, Bindings test and owning spec | Registered ResultReplayTest + ReplayFixture; same-day checkpoint regression first | Full tests, five C++ skills, analyzers, sanitizers, unchanged determinism, review; RunQuality requires clean committed HEAD |
| 11A | Offline real Engine 5/5 generation and literal saved Replay contracts; test-only synthetic snapshot | ReplayBacktestDeterminism | Full C++ gates and review; depends on 8 accepted; does not accept Task 11 desktop requirements |
| 9 | Opposite fill triangles and deterministic overlap; existing chart marker seam | Chart + ResultReplay frontend tests | Same applicable C++ gates plus fresh desktop multi-fill inspection; depends on 8 and 11A accepted |
| 10 | Chart-first collapsible/resizable layout; existing Replay sections | Replay frontend + shell geometry/keyboard tests | Same applicable gates and three desktop sizes; depends on 9 accepted |
| 11B | Real 5/5 condition-run → saved result → Replay in fresh desktop | Backtest/Replay frontend tests and actual app operation | Same gates and actual desktop date-only scenario; depends on 10 accepted; completes Task 11 only with every scenario recorded |

Checks classified from `Docs/Specs/CiDevFlow.md` and `.github/workflows/ci.yml`:
all registered Qt tests, full-tree standards/format, clang-tidy, cppcheck, IWYU,
scan-build, non-Qt ASan/UBSan/LSan and TSan, changed-line 98%/branch 90% are
implemented merge-blocking. Focused CTest is implemented local interim evidence.
Canonical fixture comparison, mutation automation, Windows and Clazy remain
Planned / not merge-blocking. Branch protection and fresh remote CI are external
/ unverified. No check is weakened or reclassified to accept an unfinished slice.

#### Slice 8 historical interim evidence — not acceptance at that time

- Baseline registered `ResultReplayTest` + `ReplayFixture` command: exit 0,
  21/21, 4.23s, same baseline HEAD before new tests.
- RED 1: `cmake --build --preset qt-dev --parallel` passed; registered focused
  command returned exit 8, 21 passed / 1 failed, 4.47s. New
  `sameDayDailyBucketExcludesBarsBeforeFirstTrustedCheckpoint` independently
  expected open 103, low 102, volume 3400. Actual 101/100/5700 for both
  Completed and Failed (6 assertions). Hourly remained correct.
- GREEN 1: Daily skips pre-checkpoint bars before aggregation, not afterward;
  focused command exit 0, 22/22, 4.14s.
- RED 2: registered `dailyFillsExcludeRecordsBeforeTrustedSelectedBarWindow`
  returned exit 8, 1 failed, 0.05s: Daily returned an earlier buy and trusted
  sell rather than only the literal trusted sell. Hourly passed its assertions.
- GREEN 2: Daily fill bounds now use the first/last trusted selected bar
  timestamps. Focused command exit 0, 23/23, 4.23s. Log:
  `Output/Task8FocusedGreen.log` (before the additional range assertions).
- Added coverage, not RED: strengthened the existing exact-span test to select
  `[00:00,01:00)` with actual before-start and at-end sentinel bars. Assert
  literal Daily OHLCV, no extra candles/fills, Partial, saved portfolio and
  terminal stepping. Full tests below include these assertions.
- `VCPKG_ROOT=Output/Vcpkg ./RunTest.sh`: exit 0, 213/213, 57.92s;
  `Output/Task8RunTest.log`. Existing duplicate-library linker warning remains;
  do not claim globally warning-free output.
- Scoped production clang-tidy: `/opt/homebrew/opt/llvm@18/bin/clang-tidy -p
  Output/analysis Src/Bindings/Replay/ResultReplay.cpp`, exit 0;
  `Output/Task8ClangTidy.log`. It reports generated non-user warnings and
  existing NOLINT suppressions, no displayed project finding. This is not the
  whole-project analyzer gate.
- Touched C++ files formatted with LLVM 18; `git diff --check` passed. Source,
  regression and owning-spec hunks self-reviewed. No new dependency, public API,
  schema, canonical hash rule or financial record mutation. No new important
  decision entry is needed for this reversible presentation-boundary repair.
- Five C++ skills applied: modern style (C++20/ranges/naming/format), OOP
  (existing private adapter and public seam retained), performance (binary
  portfolio lookup, no extra allocations or rewind rebuild), thread safety
  (N/A — no new resource, shared state, synchronization or Qt delivery changes;
  existing owned request/cancellation preserved), static analysis (scoped tidy
  above, complete gate pending). Sanitizer presets do not build this Qt Bindings
  path by default; old non-Qt results are not new-code sanitizer evidence.
- Framework-free existing 10,000-Hourly-frame probe via
  `Output/qt-dev/Tests/bte_result_replay_tests
  --gtest_filter=ResultReplayTest.tenThousandHourlyFramesUseIndexedSeekAndBoundedVisibleWindow
  --gtest_output=xml:Output/Task8ReplayPerformance.xml`: exit 0, one test,
  open 252ms, seek 0µs, model traversal 84µs, peak 52688KiB. Same macOS host
  and qt-dev preset as baseline; one sample only, not a Daily or UI rendering
  throughput measurement or optimization claim. Daily retains one pass and
  indexed record lookup; full performance acceptance remains pending.
- Complete `./RunQuality.sh --base
  e3de43bba676a8ce63953b04a49d859df8897b6e --head HEAD`: exit 2,
  "requires a clean working tree so results describe the committed HEAD".
  No full standards/analyzer/coverage pass is claimed. Commit permission was
  required at that attempt and subsequently granted; no temporary commit, stash
  or gate workaround was used.
- At that interim point Tasks 9–11 remained Planned / Not run, with no fresh
  desktop multi-fill test, comprehensive review, remote CI, commit or push.
  Slice 8 could not be accepted before required evidence became green.

#### Post-merge checkpoint — 2026-09-16

`git fetch origin main` verified #70 merged at the new base above. A direct tree
diff against the old HEAD was empty. New branch creation preserved all four
scoped modified files; unrelated root untracked files were left untouched.
Three independent review axes were then started for this slice. No later slice
was started while the required full quality result was pending.

Code/docs axes found no new blocking finding. Test axis identified a P2
same-day diagnostic suffix coverage gap; added Daily literal-prefix assertions
to the existing Engine-independent ReplayFixture and a separate persisted-fill-
after-last-checkpoint test. These additions are Added coverage, not historical
RED. Follow-up test review found the P2 resolved in intent; no remaining Task 8
test-axis finding. `cmake --build --preset qt-dev --parallel` and focused
`ResultReplayTest` + `ReplayFixture` passed exit 0, 24/24, 4.15s
(`Output/Task8ReviewFocused.log`). Earlier 213/213 is not this new 214-test suite;
the exact checkpoint full rerun below includes these additions.

Current working-tree ASan/UBSan preset passed 180/180, 48.61s
(`Output/Task8Sanitize.log`); TSan passed 180/180, 100.17s
(`Output/Task8Tsan.log`). Both are non-Qt and do not instrument the modified
ResultReplay path. Working-tree standards passed for 717 added lines before
this ledger addition; diff whitespace validation passed. These were interim
results, followed by the authorized committed checkpoint below.

#### Slice 8 accepted checkpoint — 2026-09-16

`a098577b5b1c0468109fc8b8ffb3eb9d771c2759` was clean throughout the sequential
full commands: `VCPKG_ROOT=Output/Vcpkg ./RunTest.sh` exit 0, 214/214, 55.54s
(`Output/Task8CheckpointRunTest.log`), then complete non-fast
`VCPKG_ROOT=Output/Vcpkg ./RunQuality.sh --base 84e3a135b18bdddb8134713df0b6896be0cc1a37 --head HEAD`
exit 0 (`Output/Task8CheckpointQuality.log`). Full standards/format, clang-tidy,
cppcheck, IWYU, scan-build and coverage completed; coverage tests 214/214,
58.60s, changed lines 6/6 and branches 2/2. Generated third-party warnings and
existing suppressions remain visible; this is not a globally zero-warning claim.
All three review axes completed, no remaining Task 8 P0–P2 finding. R5 accepted
only for this boundary repair. Tasks 9/10/11, broader R-items, new desktop UX,
remote CI and whole-plan acceptance remain unverified/open.

#### Slice 11A interim evidence — not acceptance

Added `Tests/Fixtures/ReplayMultiFill/SYN.csv`: fourteen synthetic hourly bars,
no imported fill records. Real snapshot selection `[01:00,13:00)` feeds the
public Engine recording seam. Initial build failed because this integration
target did not expose the Engine header; added its explicit test-only
`bte::engine` dependency in existing registration. This build failure is not a
behavioral RED. No production Engine, Strategy, hash or snapshot policy changed.

The first literal 5/5 next-open test passed 1/1, 0.04s; the stored-record and
replay contract test passed 1/1, 0.07s. Final focused `ReplayBacktestDeterminism`
passed 7/7, 0.27s (`Output/Task11AFocused.log`). Expected buys/sells, fixed prices,
amounts, ten $0.10 costs and twelve portfolio slices are independently worked
literals. Repeat runs have different IDs and equal canonical records/hash;
Hourly and Daily compare literal OHLCV, fill times/counts and saved portfolios.
End=12:00 retains five buys/four sells and ten shares; end=03:00 leaves a last
buy signal unfilled rather than consuming excluded data. Invalid plans leave
Staging/catalog empty; no-signal plans replay zero trades and unchanged cash.
These assertions are Added coverage of existing behavior, not fabricated RED.

Test temporary roots now use unique RAII `QTemporaryDir` ownership rather than
deleting a process-shared fixed path. Five C++ skills applied: modern naming,
typed chrono/fixed-point inputs, existing public seams, reserved bounded test
vectors, no new shared state or production hot-path allocation. Performance
optimization N/A — no production hot path changed. Default sanitizer presets
exclude this Qt-linked integration target; full sanitizer gates still required
but cannot be described as directly instrumenting these new tests. Touched C++
formatted with LLVM 18, working-tree standards and diff whitespace passed.
Working-tree full RunTest passed 220/220, 63.88s; ASan passed 180/180, 53.14s,
both before the negative-oracle review strengthening below. Exact-commit full
gates and final independent review are pending. No app operation,
triangle inspection, layout or Task 11 desktop acceptance is claimed.

Tests review identified a P2: reopening ResultStore before checking Staging
could hide a forbidden empty staged artifact through recovery. Corroborated
the existing recovery path; added immediate Staging/Results/Quarantine emptiness
assertions before reopen and retained checks afterward. This strengthens Added
coverage without claiming a production fix or historical RED. Docs review found
one stale current R5 limitation; corrected it to the accepted checkpoint while
keeping the historical baseline finding. Code/build review had no new P0–P2.
Follow-up tests review confirmed its P2 resolved; focused rerun passed 7/7,
0.27s (`Output/Task11AReviewFocused.log`). Non-Qt TSan completed 180/180,
102.19s (`Output/Task11ATsan.log`). Independent docs follow-up did not complete
because its agent hit an account usage limit; root verified the corrected R5
row, but does not label that failed follow-up as a completed independent review.
Exact committed full gates remain pending; 11A is still not accepted.

#### Resumed 11A checkpoint preflight — 2026-09-16

Focused tests passed 7/7, 0.30s. Fresh working-tree RunTest passed 220/220,
58.54s (`Output/Task11AResumedWorkingRunTest.log`), exit 0. Three independent
code/docs/tests reviewers completed. Docs status contradictions were corrected
and independently rechecked; no remaining checkpoint-scoped P0–P2 finding.
No production behavior changed in this 11A checkpoint. Explicit-save approval
is recorded in the owning Product/Engine specs and living lifecycle decision,
but that lifecycle is not implemented. Task 20 records the snapshot startup
blocker without claiming an exact environment reproduction or correction.
Checkpoint commit and exact-commit full gates are next; 11A acceptance, desktop
11B, R1–R4/R6/R7/R8/R9 and all new UI/history features remain pending.

#### Slice 11A accepted offline checkpoint — 2026-09-16

`6b9b3e7d83519e6cfbe5a36136879d3a672f5280` remained clean for the sequential
exact-commit workflows. `VCPKG_ROOT=Output/Vcpkg ./RunTest.sh` exit 0,
220/220, 53.82s (`Output/Task11ACheckpointRunTest.log`). Complete non-fast
RunQuality with fixed base `84e3a135b18bdddb8134713df0b6896be0cc1a37` first
returned exit 1: all analyzers completed, but coverage's locked GoogleTest
fetch failed with **Could not resolve host: github.com**
(`Output/Task11ACheckpointQuality.log`). This failure is retained, not relabelled.
After approved network access, the same complete RunQuality on unchanged HEAD
returned exit 0 (`Output/Task11ACheckpointQualityRetry.log`), including all
analyzers/scan-build and coverage tests 220/220, 57.64s. Changed source lines
6/6 and branches 2/2 are the existing Task 8 production diff; they do not measure
semantic coverage of the new 11A tests. Generated/third-party warnings remain
visible; gcovr decision analysis is heuristic, not proof of correctness.

Fresh same-checkpoint ASan/UBSan passed 180/180, 51.13s
(`Output/Task11ACheckpointSanitize.log`); TSan passed 180/180, 108.63s
(`Output/Task11ACheckpointTsan.log`). Both exclude Qt and therefore do not
instrument the new Qt-linked integration target. Three independent code/docs/
tests axes completed; docs P2 status contradictions were fixed and rechecked.
11A is accepted only as offline real-Engine generation and literal saved-Replay
coverage. Task 11B and whole Task 11 are not accepted. No new triangles, larger
layout, save/Results/history/unit UI, snapshot startup fix, push, new PR or
desktop operation is claimed. This subsequent ledger edit is documentation
only and does not turn results on `6b9b3e7` into results on a later HEAD.

#### Slice 9 accepted marker checkpoint — 2026-09-22

`e43c7fbaed24d44cf7f5fa79495f8dad1d750f74` renders a green upward buy ▲ and
red downward sell ▼ as separate pixel-space glyphs while retaining each fill's
price in the chart series and its exact timestamp/price in tooltips, persisted
results and trade rows. Series x values anchor to the containing candle.
Deterministic offsets
keep ten coincident fills individually visible. Daily presentation anchors a
fill to its containing candle without replacing the persisted fill time.
Invalid, stale and out-of-window marker paths are covered.

The new zoom-viewport regression first failed: changing the time axis left a
previously visible glyph on screen because synchronous `rangeChanged` handling
ran before Qt applied the new coordinate transform. Queued marker rearrangement
made that test green while preserving the source financial point. Focused chart
and Result Replay suites passed 2/2. Exact-commit `./RunTest.sh` passed 220/220,
54.59s. Complete non-fast `RunQuality.sh` against fixed base
`84e3a135b18bdddb8134713df0b6896be0cc1a37` passed clang-format/project
standards, clang-tidy, cppcheck, IWYU, scan-build, coverage tests 220/220 in
61.18s, changed lines 113/113 and branches 16/16. Earlier failed gate attempts
remain evidence: IWYU exposed missing direct includes, clang-tidy rejected the
deprecated `<math.h>` suggestion, and the first coverage run reported 97%
(three unexecuted viewport-hide lines); none is relabelled as success.

An isolated macOS desktop run used snapshot
`7a7bc5ce917e30685c7d1ecb38702d22b4904d2d52d1448e373620247156c094b` and
actual Selectable Conditions (`Close < 100` buy, `Close > 100` sell, capital
10000, quantity 10). Result `d025faea0e6c6da0217a3ef0f24e5349` contained
five buys and five sells. Hourly showed all ten opposite triangles; Daily kept
ten separated glyphs on the aggregate candle and all ten exact-time rows.
An initial Close-change-percent attempt produced only 4/4 because the selected
range had no prior reference bar; it is retained as a failed scenario, not
claimed as the accepted 5/5 run. Normal Launch.sh, larger layout, Task 11B and
final independent whole-branch review remain open. Qt sanitizer instrumentation
is unavailable in the checked-in sanitizer presets.

**Decision-record scope:** Task 8's trust-boundary repair and 11A's added
coverage introduce no dependency, module or schema-framing change. Reversible
layout/marker presentation does not require a new important-decision entry.
The subsequently approved explicit-save publication policy does constrain
lifecycle: it is recorded in the existing living lifecycle entry and owning
Product/Engine specs as approved, not implemented. Remaining draft retention,
recovery and other compatibility choices require separate confirmation.

## 9. Newly requested scope — recorded before implementation

**Authority:** the user requested that all recent items first be added to this
plan. This section records requirements, current facts, testable proposed
contracts and explicit decision gates. It does not change the owning specs or
living decisions, approve unresolved choices, or claim implementation. No new
source/test/build change, app operation, commit, push or PR is part of this
docs-only update. Existing uncommitted 11A work is preserved, not accepted anew.

### Requirements and unresolved decisions

| Request | Current fact | Proposal / decision required before code |
| --- | --- | --- |
| Backtest has Save and Save & Replay | At this plan-only baseline, `runPersistedBacktestConfiguration` promoted a result and the footer only opened its ID | The approved recoverable draft and explicit publication flow is now implemented in the current worktree; see Task 14's pending acceptance evidence |
| Results can select, view, delete and Open Replay | Results is a shell placeholder; backend list/open/Trash/restore/purge exists | Read-only details plus exact-ID Replay. Recommend recoverable **Move to Trash**; permanent delete and automatic purge require separate approval |
| Every indicator needs a understandable unit | Backend has price/volume/percent/scalar domains; editor offers names and bare thresholds | Show units, scale, output description and period meaning next to each selected metric; do not change mathematical domains merely to fix labels |
| Up/down X% from yesterday's close | Existing **Close change %** means prior actual bar, not prior trading day | Recommend explicit **Previous trading-day close** reference. Confirm calendar/timezone, regular versus extended-session close and current comparison price |
| Up/down from a historical high within a time window | No dedicated historical-reference condition in current editor | Expose lookback, high/low versus close extrema, reference and signed percentage. Confirm what “up from the high” means rather than silently substituting rise from the low |
| Break one-month low/high | Current Donchian upper-to-fixed-threshold choice is not a dedicated historical breakout action | Confirm trailing calendar month versus trading sessions/bars, current close versus high/low, strict threshold versus crossing event, and exclusion of current bar |
| Long selected range shows few candles | Screenshot later captures AAPL, 2018-05-01–2024-05-16, Backtest Bars 23777; sparse Replay count/result ID still absent | Diagnose source coverage, selection, processed timeline, Daily aggregation and visible-window counts separately. Backtest's large count does not prove Replay correctness; R8 remains unconfirmed |
| No Backtest Replay button when using Launch.sh | Root script builds its own checkout; root branch lacks CTA while issue10 worktree has it | Task 19 aligns launch instructions/version provenance with the accepted delivered branch; do not secretly run an unfinished worktree |

### Task 13 — Diagnose long-range sparse candles before claiming a fix

**Status:** Original sparse-Replay report not reproduced on the current saved
AAPL run; investigation remains open, and no data-selection fix is claimed.
The actual 2018-05-01–2024-05-16 Selectable Conditions run was explicitly
saved as `7e1607c01fa2da46cd797c6edb883ba2`. Its immutable result has 47
selected spans totaling 23,777 rows, 23,777 persisted portfolio checkpoints
and 103 persisted fills. Replay reported 23,777 Hourly frames and 1,522 Daily
buckets; terminal Hourly and Daily views reached all of them. This disproves
truncation for this exact new run, not necessarily the user's earlier binary
or unsaved run. Task 21 supersedes this task's historical “show all future
candles at the first cursor” wording: the accepted UI progressively reveals
one through N selected candles without truncating the total range.
**Inspect / modify only after cause is proven:**
`Src/Bindings/Backtest/BacktestSessionVm.cpp`,
`Src/Bindings/Replay/ReplayDataLoader.cpp`, `Src/Bindings/Replay/ResultReplay.cpp`,
`Src/Backend/Data/Private/ReleaseSnapshot.cpp`,
`Src/Frontend/Backtest/BacktestTab.cpp`, `Src/Frontend/Replay/ReplayTab.cpp`.
**Tests:** `Tests/Unit/Data/UnitTest_ReleaseSnapshot.cpp`,
`Tests/Unit/Bindings/UnitTest_ResultReplay.cpp`,
`Tests/Unit/Frontend/UnitTest_ReplayDataLoader.cpp`,
`Tests/Unit/Frontend/UnitTest_BacktestSessionVm.cpp`,
`Tests/Unit/Replay/UnitTest_ReplayBacktestDeterminism.cpp`.
**Interfaces:** configured date range → immutable selection identity → real
Engine processed bars → saved exact spans → Replay total/cursor/window.

- [ ] Capture symbol, immutable snapshot ID, chosen inclusive UI dates, converted
  half-open UTC boundaries, result ID/status, active Hourly/Daily mode, displayed
  count and expected count. Reproduce the actual app scenario when available;
  synthetic coverage alone is not reproduction of the user's report.
- [ ] Start from the supplied screenshot facts: AAPL, Start 2018-05-01, End
  2024-05-16, capital 100000, shares 10, ALL buy prior-bar close change >2%,
  ALL sell prior-bar close change <-2%, completed with Bars 23777 and position
  10. This is Backtest evidence only, not an observed sparse Replay timeline.
  Capture the corresponding result ID, snapshot/build identity and actual
  Replay total/visible counts before deciding which boundary is wrong. If that
  root version ran an in-memory, nonpersisted Backtest, record Result ID as
  unavailable rather than inventing one or linking a different saved run.
- [ ] Compare six quantities: source rows in requested range, selected rows,
  processed checkpoints, total Hourly frames, Daily buckets, visible candles at
  current cursor. Read actual APIs/records; do not infer total data from a screenshot.
- [ ] Use an independent literal fixture: 600 hourly rows, select `[100,550)` →
  450 rows; before-start and at-end sentinels must be excluded. First and terminal
  Replay cursors both show all 450 candles; only fills/portfolio/log advance. For
  a 600-frame selection, all 600 candles remain navigable and seek reaches 0/599.
  Cover inclusive UI end-day conversion, UTC transitions, leap day, missing
  sessions, sparse source rows, valid empty selection and diagnostic cutoff.
- [ ] Distinguish intended behavior from defects: Daily combines selected hours
  into buckets over the same complete saved range. Opening exposes the complete
  candle context but not future fills, portfolio checkpoints or trade rows.
  Never fabricate missing hours, silently truncate history or widen saved spans.
- [ ] If selection/processing is wrong, record a permanent failing regression
  before the minimal correction. If only source coverage/window interpretation
  explains it, record that evidence and propose a clear selected/processed/total/
  visible-count message. Any new message gets its own UI RED/GREEN cycle.
- [ ] Run affected Data/Bindings/Replay tests and fresh app long-range operation;
  record the proven cause, correction or honest unresolved state. R5 acceptance
  is not evidence that R8 is fixed. A genuine data-loss defect blocks later
  affected workflow acceptance until corrected.

### Task 14 — Backtest Save Result / Save & Replay

**Status:** Implemented on `a5a43cf` with follow-up review/coverage commits
through `15dfcab`; final acceptance pending. Exact committed `15dfcab`
RunTest and full RunQuality pass 232/232 and 98% changed-line coverage.
Starter desktop operation is verified, not multi-fill or full review. No PR is
claimed.
**Maintainer confirmation (2026-09-16):** record recoverable execution drafts,
including trustworthy diagnostic records, while the run is executing. A draft
is not an active saved result. Unsaved drafts survive restart until explicit
Save or Discard. Starting another run preserves the prior draft in an
**Unsaved Runs** list; exit neither publishes nor discards it. **Save Result**
validates and publishes that run
to Results without navigation; **Save & Replay** performs the same idempotent
save and opens its exact ID. Neither action reruns Engine. Approval of this flow
is not acceptance of R1–R3 or any implementation.
**Modify:** `Src/Frontend/Backtest/BacktestTab.cpp`,
`Src/Frontend/Include/Bte/Frontend/BacktestTab.h`,
`Src/Bindings/Backtest/BacktestSessionVm.cpp`,
`Src/Bindings/Include/Bte/Bindings/BacktestSessionVm.h`,
`Src/App/MainWindow.cpp`, `Src/App/MainWindow.h`,
`Docs/Specs/BacktestReplayProduct.md`, `Docs/Specs/EngineReplayPnL.md`.
**Tests:** `Tests/Unit/Frontend/UnitTest_BacktestTab.cpp`,
`Tests/Unit/Frontend/UnitTest_BacktestSessionVm.cpp`,
`Tests/Unit/Frontend/UnitTest_MainWindow.cpp`,
`Tests/Unit/Results/UnitTest_ResultStore.cpp`.
**Interfaces:** one completed Engine timeline → approved persistence operation
→ one promoted Result ID; Save & Replay adds shell navigation with that same ID.
The implemented ResultStore/Bindings draft APIs are subject to final review.

- [x] Confirm automatic versus explicit save/promotion, completed versus
  diagnostic handling, unsaved run replacement, app exit/restart recovery and
  immutable segment retention for any pending draft. Explicit saving must not
  discard execution-time diagnostic durability or conceal R1–R3. Update owning
  specs and qualifying living decision before changing lifecycle.
- [x] RED for two distinct buttons: unavailable before a usable run, disabled
  during run/save, success only after validation/promotion. **Save Result** stays
  on the page; **Save & Replay** opens exactly the saved result. Save failure
  shows the structured error and never navigates.
- [x] Test repeated Save, double clicks, Save then Save & Replay and switching to
  a new run: one run has one saved ID/artifact, no second Engine execution, no
  duplicate fills/hash mutation, no stale ID selected. Saved run configuration
  remains the original, not edits made afterward. Storage idempotency,
  Save-then-Replay, failure double click and prior draft preservation have
  coverage; post-run configuration edits and success double click gained
  focused assertions before `a5a43cf`. A multiple-draft Save Result → Save &
  Replay regression
  first failed because the refresh selected a different draft; it now passes
  without changing the saved Result ID. A live writer versus a second store
  open regression also failed before same-process recovery coordination and
  now passes. Cross-process concurrent opens remain unverified.
- [ ] Complete acceptance of the implemented lifecycle: rerun full gates on an
  exact commit, review all storage/retention paths, and perform desktop
  keyboard/error and multi-fill operation. The current starter Save/Replay
  desktop path and focused/full working-tree tests do not complete this item.

### Task 15 — Results selection, read-only view, delete and Open Replay

**Status:** In progress, not accepted. A first Results page and owned
asynchronous catalog/details/Trash requests are committed through `12958ea`.
Focused tests cover empty and corrupt catalogs, one real snapshot-backed
result's details/ID navigation, cancel-delete, Trash and restore with identical
hash, diagnostic status display, storage errors and currently open Replay
invalidation. Exact `12958ea` full non-fast quality passes 236/236 with 98%
changed-line and 93.9% changed-branch coverage. The isolated desktop app was
launched, but CUA reported the Mac locked; no Results UI operation is claimed.
Later `d8b3c76` tests covered multiple-entry ordering, three viewport sizes,
corrupt-first/valid-next catalog resilience and exact-ID Replay routing.
On 2026-10-06 the unlocked desktop app listed the saved AAPL result
`7e1607c01fa2da46cd797c6edb883ba2`; selecting its row enabled View Details
and Open Replay. View Details displayed AAPL, the exact half-open UTC range,
Completed status, $98,926.08 final equity, -$1,073.92 PnL, canonical hash,
Available data and an honest unavailable advanced-metrics label. Open Replay
opened the same ID. With the maintainer's explicit approval, the isolated app
moved that ID to Trash: Saved Results became empty, Trash listed the same ID,
and Restore returned it to Saved Results with the same ID. The UI operation
used keyboard focus and Space to select the Trash row. Full keyboard-only
traversal, hash comparison after this desktop restore, multi-entry operation
and three desktop sizes remain unverified; focused tests do not substitute
for those checks. A read-only SQLite integrity check after Restore returned
`ok`; `result_meta` retained canonical hash
`c9bb2855c55b1f0f9cc84d668c9fb7a4e13d07b8a28360612435aa639f428b6d`,
matching the value shown before Trash. The artifact still contains 103 fill
records and 23,777 portfolio checkpoints.
**Create, proposed new files (not existing APIs):**
`Src/Frontend/Results/ResultsTab.cpp`,
`Src/Frontend/Include/Bte/Frontend/ResultsTab.h`,
`Tests/Unit/Frontend/UnitTest_ResultsTab.cpp`.
**Modify:** `Src/Frontend/CMakeLists.txt`, `Src/Bindings/CMakeLists.txt` only if
new owned adapters are needed, `Src/App/MainWindow.cpp`, `Src/App/MainWindow.h`,
`Tests/CMakeLists.txt`, `Tests/Unit/Frontend/UnitTest_MainWindow.cpp`,
`Tests/Unit/Results/UnitTest_ResultStore.cpp`, `Docs/Specs/BacktestReplayProduct.md`,
`Docs/Specs/FrontendQt.md`.
**Interfaces:** existing ResultStore list/open/lifecycle → asynchronous owned
Bindings adapter → selected read-only details; shell consumes only Result ID
for Replay. Frontend never handles SQLite or recomputes performance.

- [x] Confirm recoverable Trash versus permanent removal. Active
  list **View Details**, **Open Replay**, **Move to Trash**, with explicit
  confirmation and Restore. Trash retains entries for 30 days; any permanent
  or automatic purge still requires separate approval and verification.
- [ ] RED: list saved completed/diagnostic entries; select one by ID; show actual
  symbol, range, status, stored financial summary where eligible, hash and data
  availability. Unsupported metrics are labelled unavailable, not fabricated.
- [ ] Cover no selection/empty list, multiple rows, newest-first ordering,
  corrupt/incompatible/missing-data results, typed list/open/delete errors and
  stale asynchronous responses. Invalid results cannot be opened in Replay;
  readable failure details must not block valid rows.
- [ ] Cover cancel-delete no mutation, approved Trash removal from Active,
  restore with identical records/hash, retention references, deleting the
  currently selected/playing result and selection switching during load. Stop
  and invalidate affected Replay safely; do not permanently delete market data.
- [ ] GREEN: actual Backtest save → Results View → same-ID Open Replay, then
  delete/restore in isolated app. Test keyboard-only operation and three target
  sizes. Register the new suite explicitly; existing backend lifecycle tests
  alone do not prove Results UI completion.

### Task 16 — Metric-specific units, scales and clear threshold labels

**Status:** In progress, not accepted. `236d97b` added a display-only unit,
scale, example and bar-period mapping for all 18 offered metrics in all four
condition rows; `cf65e2a` fixed a cppcheck-detected default-initialization
defect. A new UI regression first failed on the missing percent suffix, then
passed for 18 × 4 choices and unchanged numeric thresholds. Exact `cf65e2a`
`RunTest.sh` and full non-fast `RunQuality.sh` passed 236/236, 98% changed-line
and 94.3% changed-branch coverage. `d8b3c76` added UI threshold tests for 5
versus 0.05, negative/zero and ALL/ANY without rescaling. On 2026-10-06 the
desktop Selectable Conditions editor visibly showed `5.0000 %` / `-5.0000 %`
for Close change and `USD` for SMA; its accessible descriptions explicitly
distinguish prior actual bar from trading day and bars from calendar periods.
Canonical-hash comparison remains. On 2026-10-08, the rebuilt app's four
condition selectors were each traversed through all 18 choices; each exposed
a metric-specific accessible explanation and example, including signed ROC
percent points, RSI oscillator level, and OBV shares. The test-only UI
selection was restored afterward. This desktop traversal confirms presentation,
not unchanged Engine records or hashes.
The current backend numeric domains and indicator formulas are unchanged.
**Modify:** `Src/Frontend/Backtest/BacktestTab.cpp`,
`Tests/Unit/Frontend/UnitTest_BacktestTab.cpp`, `Docs/Specs/Indicators.md`,
`Docs/Specs/StrategyAuthoring.md`, `Docs/Specs/FrontendQt.md`.
**Inspect:** `Src/Backend/Indicators/Include/Bte/Indicators/StreamingIndicator.h`
and `Src/Backend/Indicators/Private/StreamingIndicator.cpp`.
**Interfaces:** selected IndicatorDefinition/output → existing
`indicatorOutputDomain` → display unit/scale/example. No formula or canonical
Strategy identity changes for a label-only correction.

| Currently offered metric | Required display meaning |
| --- | --- |
| Close, SMA, EMA, WMA, Bollinger upper, Donchian upper, VWAP | Price in the run's quote currency; current fixture/UI currency is USD |
| MACD histogram, ATR, Momentum, True range | Price difference/range in quote currency, not percent; Momentum may be negative |
| ROC and prior-actual-bar Close change | Signed percent change: threshold 5 means 5%, not 0.05 |
| RSI, ADX, Stochastic %K | Dimensionless oscillator on a 0–100 scale; backend currently classifies as percent. Explain “RSI 30” as oscillator level, not a 30% price decline |
| Volume, OBV | Shares / cumulative signed volume; OBV may be negative, not dollars |
| Any subsequently exposed Bollinger width/percent-B output | Existing scalar domain; label the verified formula scale explicitly, never infer percent from the output name |

- [ ] RED: traverse every offered metric in each buy/sell row and verify visible
  units, accessible labels/descriptions, output/period meaning and examples.
  Changing metrics updates labels immediately; period is **bars** for existing
  hourly indicators, not days or months. Warm-up absence is explained. Disable
  or hide irrelevant period controls for passthrough/prior-bar metrics so a
  number such as 14 cannot suggest an unsupported fourteen-day comparison.
- [ ] Cover threshold 5 versus 0.05, signed ROC/MACD/Momentum/OBV, zero/equality
  boundaries, invalid/nonfinite numeric inputs, metric changes and two-row
  ALL/ANY groups. Changing labels must not silently rescale a user's threshold.
- [ ] GREEN: confirm UI values map to the unchanged typed domain and produce the
  same Engine records/hash. Inspect all metric choices in fresh app, not only
  RSI. Any needed computation change is a separate approved regression slice.

### Task 17 — Up/down X% relative to previous UTC data-day close

**Status:** CSV/UTC-day predicate semantics approved on 2026-10-12 / not
implemented. The maintainer explicitly selected the existing CSV as the
standard and confirmed UTC-date grouping consistent with Daily Replay.
Every UTC date containing actual source rows counts as one data day; its
closing reference is the last chronological CSV close, including extended
hours. This supersedes the earlier official-session-close requirement and
removes the missing official-source blocker. It is not an exchange-calendar
or official regular-session close. Runtime evaluation must read the same
immutable CSV-derived snapshot, not fall back to mutable CSV or download data.
Pre-range warm-up, versioned Strategy framing and required regression/
integration evidence remain unimplemented; no Task 17 behavior is claimed.
**Modify after definition approval:**
`Src/Backend/Strategy/Include/Bte/Strategy/SelectableStrategy.h`,
`Src/Backend/Strategy/Private/SelectableStrategy.cpp`,
`Src/Bindings/Backtest/BacktestSessionVm.cpp`,
`Src/Frontend/Backtest/BacktestTab.cpp`,
`Tests/Unit/Strategy/UnitTest_SelectableStrategy.cpp`,
`Tests/Unit/Replay/UnitTest_ReplayBacktestDeterminism.cpp`,
`Tests/Unit/Frontend/UnitTest_BacktestTab.cpp`,
`Docs/Specs/StrategyAuthoring.md`, `Docs/Specs/DataLayer.md`,
`Docs/Specs/EngineReplayPnL.md`.
**Interfaces:** chronology-safe current price + approved prior UTC data-day close
reference → typed up/down-percent predicate → next-actual-open Engine order.
Preserve existing prior-bar Close change behavior as a distinct option.

- [x] Confirm previous data day versus calendar yesterday, UTC date grouping,
  last available CSV close rather than official regular-session close,
  current comparison price and whether X means at least or strictly more.
  Define pre-range warm-up without adding warm-up bars/orders to saved execution
  spans. Missing required reference means absent/nonmatching with clear reason,
  never a fabricated close or future lookup. Approved contract: use the
  prior actual UTC data date's last immutable source close, compare
  the current bar close, and treat positive magnitude X as inclusive at-least
  down/up X percent. Include extended-hours rows under this explicit CSV policy.
- [ ] Literal contract after approval: reference close 100 and current price 95
  gives -5%; 105 gives +5%; 100 gives 0%. UI takes positive magnitude X with
  separate **Down** / **Up** choices. Test exact threshold and immediately either
  side according to the approved comparison, zero, invalid negative magnitude
  and zero/absent reference.
- [ ] Use a multi-row prior-date literal oracle: an earlier regular-hours close
  is 100 but the later extended-hours close on the same UTC date is 110. On the
  next available date, 104.5 is exactly 5% down and 115.5 exactly 5% up from 110.
  Assert immediately adjacent threshold cases and multiple next-date bars with
  the same reference. This must fail if regular-hours close or prior-hour close
  is substituted. Include a UTC-midnight transition and a date with no rows.
- [ ] RED for Friday→Monday, dates with no source rows, UTC midnight, year/leap
  boundaries and pre-range reference warm-up. Exchange DST does not change UTC
  grouping. Multiple intraday bars keep the same previous-data-day reference;
  they cannot substitute the previous hour or read the current day's future close.
- [ ] Freeze an immutable snapshot, then change the original CSV: pre-range
  references and signals must remain unchanged. If that snapshot lacks required
  history, fail absent/nonmatching with the specified reason even when mutable
  CSV contains older rows; no runtime source fallback is permitted. Warm-up
  must not enter saved execution spans or add orders/fills/Replay candles.
- [ ] Assert prior-day and N-day close metrics' UI labels and tooltips disclose
  **CSV / UTC data days** and **last source close, including extended hours**.
  Extrema and breakout metrics instead disclose all source-row highs/lows,
  including extended hours. Percentage predicates disclose positive magnitudes
  and their specified inclusive comparisons; breakouts disclose strict
  comparisons. Do not call a CSV close an official exchange close.
- [ ] GREEN real Engine saved/repeated results for buy and sell groups; Replay
  remains read-only. Extend canonical Strategy framing for new fields with
  explicit compatibility review; existing saved hashes must not be reinterpreted.

### Task 18 — Historical extrema changes and one-month breakouts

**Status:** Period-gain semantics clarified / not implemented. On 2026-10-06 the maintainer
clarified that “up X%” means an increase over a chosen historical period
(for example, 30 days), while a separate condition means a drawdown of X%
from a historical high. It does not mean X% above the previous high. The
maintainer then chose **30 trading sessions** for the example: compare the
starting completed session's close with the current bar close. This is not a
30-calendar-day or prior-hour calculation. The maintainer also approved the
prior 21 completed trading sessions for one-month extrema/breakout, excluding
the current session, and a persistent strict breakout predicate: every
qualifying current K bar matches, not only the first crossing. On 2026-10-12
the maintainer selected existing CSV-derived UTC data dates instead of official
exchange sessions. A 30-day gain compares the current bar close with the last
close of the UTC data date exactly 30 available data dates before the current
date. Extrema use all rows of the previous 21 completed UTC data dates, excluding
the current date. Empty calendar dates do not consume the lookback and no missing
row is synthesized. History availability, pre-range warm-up and implementation
remain open; no official-source integration is needed.
**Modify after approval:** Strategy header/private implementation, Backtest
Bindings/editor, Strategy/Engine/Replay integration/Frontend tests listed in
Task 17, plus `Docs/Specs/Indicators.md` if a shared indicator is extended.
No new public predicate API is claimed to exist.
**Interfaces:** approved bounded chronological historical window → high/low
reference and percent/breakout signal → normal next-open Engine execution.

- [x] Confirm lookback unit (calendar month, trading days or bars), default,
  high/low versus closing-price extrema, UTC boundaries, comparison price,
  sufficient-history requirement and threshold/crossing semantics. Recommendation:
  references use only completed prior UTC data dates, excluding the current date.
  The approved **One-month (21 UTC data days)** extrema/breakout
  preset uses exactly the
  prior 21 completed available data dates, requires all 21, excludes the current date,
  uses source-day high/low and current-bar close, uses inclusive percentage
  thresholds, and uses strict breakout comparisons. Each qualifying bar
  matches; no previous-side sample is required. These are CSV-defined data days,
  not official exchange sessions.
- [ ] Provide distinct readable choices for **Up X% over selected period**,
  **Down X% from period high**, **Up X% from period low**, **Break period high**
  and **Break period low**. Do not relabel period gain as a breakout above the
  high or a rebound from the low; use the approved CSV/UTC-date anchor.
- [ ] Independent literal oracles: prior high 120/current 108 → -10%; prior low
  80/current 88 → +10%; period start close 100/current 110 → +10% once
  the approved 30-UTC-data-day period anchor is used. Historical high 100:
  current 100 is not a strict breakout, 101 is; historical low 80: 80 is not a
  strict breakdown, 79 is. Persistent comparison, not a first-cross event,
  is the approved behavior.
- [ ] RED for exact window edge, just inside/outside, repeated equal extremes,
  missing bars/sessions, month length/leap year, history budget, absent warm-up,
  invalid periods and reference zero. Adding a future high 1000 or low 1 must
  never change an earlier signal. Do not scan unbounded history per bar.
- [ ] Exclude the whole current UTC date, including already seen bars: with
  prior-21-date high 100, a current-date first bar high 150/close 101 and later
  close 101 must both match the strict high breakout. Mirror with prior low 80,
  current-date early low 1 and repeated close 79. On the next data date those
  completed extrema must enter the window; do not permanently omit that date.
- [ ] Prove extended-hours rows enter the N=30 anchor and prior-21 extrema.
  Use an anchor date with earlier close 100/later close 110; literal current
  close 121 is a 10% period gain from 110. Use prior-date extended-hours high
  120/low 80 to prove 108 is 10% drawdown and 88 is 10% rebound, rather than
  comparing only regular-hours rows. Also verify fixed-snapshot CSV mutation,
  insufficient immutable history/no fallback, and UI CSV/UTC disclosure.
- [ ] GREEN literal signals and real repeated Engine records/hash for both buy
  and sell conditions. Period inputs and threshold units must match Task 16;
  pre-range warm-up never becomes extra Replay candles. Version new functional
  Strategy fields and review any qualifying cross-module/lifecycle decision.

### Task 19 — Normal Launch.sh workflow and version provenance

**Status:** Worktree launcher provenance committed and quality-gated; final
delivered-checkout acceptance remains open.
**Observed facts:** root is `codex/implement-ci-slices` at `a66e888`; issue10
worktree is `codex/issue10-replay-ux` at `a098577` with uncommitted 11A/plan work.
Root `Launch.sh` resolves its own directory, configures/builds there, then runs
that directory's `Output/qt-dev/Src/App/stockBacktester`. It does not fetch Git,
switch branches or choose a worktree. Root Backtest source has no Replay CTA;
the worktree source creates `backtestOpenInReplayButton`. This explains the
reported behavior when root Launch.sh is used, but live process-path inspection
was blocked by sandbox permissions and no new desktop launch was verified.
The script is not proven broken merely because the checkout is a different
version. Invoking the worktree's script is a diagnostic workaround, not delivery.

**Inspect / proposed modify:** `Launch.sh`, `Src/App/MainWindow.cpp`,
`Src/App/MainWindow.h`, `README.md`, `Docs/BUILD.md`,
`Tests/Unit/Frontend/UnitTest_MainWindow.cpp`.
**Create only if script behavior changes, proposed new test file:**
`Tests/Unit/Tools/UnitTest_Launch.py`; register it explicitly through the existing
test runner before treating it as a gate.
**Interfaces:** user invokes the documented launcher → explicit checkout/build
identity → freshly built accepted app → completed saved Result ID → Replay CTA.

- [x] Agree which checkout is the supported normal launch location. Required
  delivery: merge the reviewed feature branch, then update the user's intended
  checkout with their approval; root Launch.sh builds that checkout. Do not
  change the user's branch, overwrite uncommitted files or hard-code routing to
  `.worktrees/issue10` without authorization. The repository-root checkout is
  the supported normal launch location after this reviewed feature is merged.
- [x] Decide whether launcher output/app diagnostics should show repository path,
  branch/commit and build identity. This was originally proposed; the implemented
  launcher diagnostics are recorded in the later Task 19 checkpoint below.
  Report dirty builds honestly; a visible Replay tab is not proof that the
  Backtest result-save/CTA integration exists in that binary. Launcher output
  and app diagnostics must show checkout path, branch/commit, dirty state,
  build identity and selected snapshot ID.
- [x] If launcher behavior changes, first add positive/negative/boundary tooling
  tests with a real temporary directory and stub external build commands:
  invocation from another working directory and paths with spaces target the
  script's own checkout; configure/build failure returns nonzero and never
  launches; no implicit fetch/branch switch/other-worktree build occurs. Run
  `bash -n Launch.sh` plus the registered tests; a syntax check alone is not
  behavioral acceptance. Provenance assertions were RED before the launcher
  edit, then the project-script suite passed 22/22 and `bash -n Launch.sh`
  passed. The launcher now prints checkout, branch/commit, dirty state,
  executable SHA-256 and selected snapshot ID before `exec`, exports those
  values to the app, and the Help menu exposes Build Information. A focused Qt
  test passed after an initially failing missing-action assertion. A focused
  script test verifies a clean detached checkout. An actual worktree `Launch.sh`
  configure/build succeeded and printed checkout, branch, commit, dirty=yes,
  app digest and snapshot `229c37d2...74acf`; its raw executable could not
  create a macOS window in this sandbox (`Cannot create window: no screens
  available`), so that invocation does not prove a desktop workflow.
  Exact committed `3245574` then passed the full non-fast `./RunQuality.sh`:
  clang-tidy, cppcheck, IWYU, scan-build, 236/236 coverage tests, 98%
  changed-line and 94.7% changed-branch coverage. Earlier commits failed
  first on raw `new` (project RAII rule), then on missing direct Qt headers;
  both were corrected before this pass. Opening the same built bundle via the
  desktop automation API showed Help → Build Information, with truthful
  `unavailable` values because that invocation did not come from Launch.sh.
- [ ] In the final intended checkout, close the old app, execute its documented
  Launch.sh and verify the fresh app/build identity. Perform a real Backtest;
  verify the current **Open in Replay** or newly accepted Task 14 **Save & Replay**
  is visible/enabled in the specified state and navigates to the exact saved ID.
  Opening a manually imported result is not a substitute for the normal path.
- [ ] Record invocation, directory, commit/dirty state, result ID, actual UI
  observation and errors. Full final gates and new PR use the actual submission
  commit; the user's current root binary and the unfinished worktree are not
  interchangeable acceptance evidence.

### Execution and acceptance of the new scope

- Task 13 starts with diagnosis; a demonstrated selection/data-loss defect takes
  priority over acceptance of dependent save/Results/Replay behavior. No failure
  is deferred while implementing a later affected slice.
- Task 14 has an approved storage-timing choice and requires detailed draft
  lifecycle decisions and safe Results lifecycle; Task
  15 consumes its exact saved ID. R1/R2/R3 remain open prerequisites where the
  chosen lifecycle relies on them. Task 16 can be an independent label slice
  after the current 11A checkpoint is green. Tasks 17/18 require definition and
  compatibility approval, typed Strategy execution and Task 16 unit presentation.
- Task 12 is the final re-acceptance/handoff, not finished before new requested
  requirements have explicit accepted/scoped/deferred dispositions. Do not open
  a “complete plan” PR while these requests are only proposals.
- Task 19's final normal-launch check follows the accepted save/Replay workflow
  and applies to the delivered checkout. Static launch-path diagnosis may be
  recorded now, but actual-launch acceptance cannot be checked before that.
- For each approved behavior slice: `tdd`, all five project C++ skills,
  independent review, real RED/Added coverage labels and full applicable gates.
  Verified interim commands include the following; register any new Results UI
  suite before relying on it. A regex with no matching test is not success.

```bash
cmake --build --preset qt-dev --parallel
ctest --test-dir Output/qt-dev -R 'SelectableStrategyTest\.|ReplayBacktestDeterminism\.|ResultReplayTest\.|ReleaseSnapshotTest\.|bte_backtest_frontend_tests|bte_backtest_vm_tests|bte_result_replay_frontend_tests|bte_app_shell_tests' --no-tests=error --output-on-failure
```

On the exact committed submission HEAD, run `./RunTest.sh` then complete
`./RunQuality.sh --base 84e3a135b18bdddb8134713df0b6896be0cc1a37 --head HEAD`,
both sanitizer presets and applicable performance/desktop checks. These are
future acceptance commands, not executed evidence for this document change.
This plan-only revision requires local-link/path/status/contradiction and diff
whitespace validation; build/runtime/static analysis/sanitizers are
**N/A — docs-only**, not newly passed. Exact implementation results stay in
the earlier dated ledger and do not accept Tasks 13–19.

**Plan-update validation (2026-09-16):** root self-reviewed request coverage,
decision gates, current/proposed distinctions and existing/planned file paths.
Existing Markdown-linked local spec/decision files resolve; the referenced
canonical-result anchor is present. `git diff --check` passed. This turn changed
only this Markdown plan; pre-existing 11A source/test/build modifications and
the untracked synthetic fixture were preserved. No implementation check was
rerun or credited as new evidence for Tasks 13–19. All new task checkboxes remain
unchecked, with approval/reproduction dependencies explicit.

## 10. Complete request inventory and final completion rule

This is the single consolidated checklist for all requests in this conversation,
not another implementation plan or a replacement for the dated evidence ledger.
“Done” below means scoped acceptance, not merely added code or written planning.

| User requirement / discovered prerequisite | Task / tracked finding | Current disposition |
| --- | --- | --- |
| Replay uses saved selected backtest bars; trusted Daily prefix/fills | 8 / R5, 13 / R8 | 8 accepted at a098577; long-range report still needs diagnosis |
| Buy green ▲, sell red ▼, exact fill time/price, readable overlaps | 9 | Accepted at e43c7fb; exact gates and isolated real-Engine 5/5 desktop evidence recorded |
| Bigger chart, collapsible settings, adjustable log/chart, three sizes | 10 | Automated implementation and prior full quality gate passed at `8256300`; the isolated Mac is now usable, but three-size desktop acceptance remains open |
| Actual Selectable Conditions Backtest gives at least five buys/five sells | 11A + 11B | Offline 7-test coverage accepted at 6b9b3e7; fresh desktop acceptance missing |
| Backtest Save Result and Save & Replay | 14 | Durable draft and explicit publication implemented in current worktree; final quality/review and multi-fill desktop acceptance pending |
| Results select/view/delete and Open Replay | 15 | Isolated desktop selected a named six-year Result and opened its exact ID in Replay; delete/restore, keyboard and three-size acceptance remain |
| Every offered indicator has correct unit, scale, example and period meaning | 16 | All 18 offered metrics have a tested display mapping and exact `cf65e2a` green full gate; explicit numeric-boundary/hash cases and fresh desktop inspection remain |
| Relative to prior data-day close: down/up X% | 17 | CSV/UTC data-day convention approved; previous available date's last source close, including extended hours; implementation pending and distinct from prior-hour percentage |
| Time-window historical high/low changes, up/down and monthly high/low break | 18 | CSV/UTC convention approved: 30 available data-date close-to-close gain, prior 21 completed data dates excluding the current date, and persistent strict breakouts; implementation/warm-up remain open |
| Normal Launch.sh path exposes the delivered Backtest Replay action | 19 | Worktree launcher implementation and full gates are accepted at `c28ddec`; the built app exposes Open Replay, while the final delivered-checkout identity check remains |
| Backtest cannot run: Snapshot open request is invalid | 20 / R9 | Launcher/snapshot scope is accepted at `c28ddec`; actual AAPL 48-bar Backtest and same-result Replay succeeded, while the multi-fill Save/Results final scenario remains |
| Hourly/Daily progressively replay every selected K-line without a 500-frame truncation; fills appear with their bars | 21 / R10 | Accepted at `05c23eb`: initial prefix is one candle, cursor 500 presents 501 candles, rewind shrinks candles/fills, timeframe switching preserves the current UTC date, full gates pass, and actual Hourly/Daily app operation is recorded |
| Buy/Sell glyph is visibly beside the exact containing K bar | 22 / R11 | Accepted at `6f6fc86`; exact marker values remain unchanged while presentation glyphs use containing-candle x and low/high-relative placement |
| One scroll gesture must not make Replay candles disappear | 23 / R12 | Reproduced on actual AAPL Replay; focused regression and repair pass, final gates pending |
| One bar per hour/day; price candles above volume; readable time/price/volume axes | 24 / R13 | Separate aligned price/volume panes, 24-pixel default slots and focused tests pass; isolated AAPL reached 23,777/23,777 and Follow latest was exercised, but long-seek responsiveness, resize, three-size checks, exact-data comparison and final gates remain open |
| Readable saved Result name and later rename without changing identity | 25 | Naming and rename were exercised in the isolated app; exact-HEAD `12fe8eb` quality passed, but later edits and whole-plan gates remain open |
| Chart density, Follow latest and history zoom on long playback | Replay viewport plan / 24 | The latest repair has focused RED/GREEN and isolated-app checks; width and Follow latest improved, but final resize/performance/data acceptance is open |
| Small visible clearance between trade triangles and candle wicks; no compressed final view with Max | 26 | Eight-pixel clearance confirmed; spacing/height-resize and intrinsic-label repairs pass exact full gates through d209d21, with isolated AAPL Max/zoom/Follow checks. Responsiveness and complete three-size layout acceptance remain open |
| Persistence, retention, mid-run durability, rounding, golden oracle, honest capabilities | R1–R4/R6/R7 | R4 scoped repair accepted at b5be0c8; R1/R2/R3/R6/R7 remain open and are not hidden behind UI completion or green tests |
| Publish all in-scope workspace changes and reconcile local branches/PRs | 27 | In progress; #68/#69/#70 are merged, older seven-fix branch is superseded, Issue 10 follow-up and root untracked test disposition remain open |
| Multiple Buys in one displayed K bar use one Buy glyph | 28 | Scoped Buy-only checkpoint accepted at d209d21 with regressions, full gates, review and isolated AAPL desktop checks; Sell remains per-fill; whole-plan acceptance remains open |
| Complete tasks, merge a CI-green PR, then remove the confirmed non-main branches | 29 | Draft PR #76 published as the requested consolidated checkpoint; CI red, not merged, no branch deleted; final content reconciliation and cleanup scope remain open |
| Fully inspect/operate app, track honestly, create new PR only after finished | 12 | Needs changes; the later explicit checkpoint-publication request is fulfilled by Draft #76, not whole-plan completion or a universal bug-free promise |

- [x] Before implementation resumes, confirm outstanding product/lifecycle/history
  definitions through the governance decision process. Plan-only authorization
  does not choose them. Continue one green slice at a time; preserve prior work.
- [ ] Every accepted task has implementation revision, test seam, actual RED or
  Added coverage, GREEN/full command exits, review outcome, desktop evidence when
  required, and explicit limitations. A plan checkbox cannot replace any of these.
- [ ] Final scenario: intended Launch.sh → actual multi-fill condition-run →
  Save Result → Results view → same-ID Open Replay; repeat using Save & Replay;
  verify triangles, portfolio, long-range counts, Hourly/Daily controls,
  resizing/result switching and approved delete/restore behavior.
- [ ] Run required full gates on the exact submitted commit and resolve blocking
  findings. A final PR must list accepted scope and explicit maintainer-approved
  deferrals; no implicit exclusion of new requests or broader correctness gaps.

### Task 20 — Valid immutable snapshot configuration in normal startup

**Status:** Launcher/snapshot implementation and the actual starter-result
workflow are accepted at `c28ddec`; the later multi-fill Save/Results scenario
remains pending, so Task 20 as a whole is not complete.
**Evidence:** the second screenshot reports **Cannot run — Snapshot open request
is invalid** for AAPL, 2018-05-01–2024-05-23, capital 100000 and shares 10.
The configured Backtest reads `BTE_DATA_SNAPSHOT_ID`; snapshot opening rejects
an empty store path or an ID other than 64 lowercase hexadecimal characters
before loading a manifest or executing Engine. Launch.sh currently does not
prepare or select a snapshot. The screenshot does not expose its process
environment, so missing ID is the evidenced likely cause, not a verified capture
of the user's exact environment.

- [x] Diagnose fresh normal startup, capture non-secret configuration and exact
  structured failure, and distinguish missing ID, invalid ID, missing manifest,
  corrupt data and valid selection with no bars.
- [x] Define a discoverable read-only snapshot selection/configuration path.
  Keep snapshot building a separate developer/release process: no runtime CSV
  fallback, provider download, fabricated snapshot or hidden test fixture.
- [x] Add startup/Bindings regression tests before the correction; errors explain
  what configuration is missing and how to select an available valid snapshot.
  A failed new run cannot expose a stale saved-result action as its own result.
- [ ] Verify Launch.sh → valid immutable snapshot → actual condition Backtest
  → approved save → same-ID Replay in the app. Record actual ID/counts/errors.
  No synthetic backend success substitutes for the normal startup workflow.

Implementation evidence: `Launch.sh` now either validates and preserves an
explicit `BTE_DATA_STORE`/`BTE_DATA_SNAPSHOT_ID` pair or runs the separate
snapshot-builder executable before app startup. Partial configuration, invalid
identity, missing manifest, invalid builder output and builder failure are
covered and fail before app execution. With neither variable set, the developer
workflow builds the three symbols offered by the UI under `Output/DevData` and
passes the exact content-addressed identity. The Data parser regression first
failed because the builder only accepted an eight-column test header while the
tracked extracts are the canonical seven-column form; the corrected parser
supports both without permitting arbitrary extra columns.

Actual worktree evidence used snapshot
`229c37d29a99f57f4ee22c1c32342e6ce903bc2cb6278d207c55718128274acf`.
An AAPL starter Backtest completed 48 bars and persisted result
`0bbe3c1229e813cdcf6f31859a0a1456`. Open Replay displayed one real candle at
1/48, then Step reached 12/48 with visibly distinct OHLC bodies/wicks and the
Buy glyph below its containing candle. This proves the earlier repeated shapes
came from the cyclic stress fixture, not production OHLC rewriting. It does not
complete the required Selectable Conditions multi-fill Save/Results scenario.

Verification on exact implementation revision `c28ddec` passed RunTest 221/221
in 62.57s. The non-fast RunQuality gate exited 0; its coverage run passed
221/221 in 64.42s with 279/279 changed lines and 49/52 changed branches, and
clang-tidy, cppcheck, IWYU and scan-build passed. The first full-quality attempt
stopped because `VCPKG_ROOT` was not configured. After selecting the verified
pinned checkout, the next attempt correctly rejected `readSourceBars` at
cognitive complexity 29 over the limit 25. Extracting the exact source-layout
parser removed that finding without a suppression; neither failed attempt is
reported as passing.

### Task 21 — Complete-range progressive Hourly/Daily playback

**Status:** Accepted at `05c23eb` after corrected tests, full gates and actual-app
operation. `c98e375` remains recorded as technically green but superseded by the
maintainer's desktop rejection.
**Evidence:** the supplied screenshot shows a one-day synthetic result in
**Daily (UTC)** as one aggregate candle with all ten fill glyphs spread across
that candle. A one-day Daily aggregate is mathematically expected, but it is not
valid acceptance evidence for the requested Replay. Later actual-app operation
used result `c47c9a10ee7c18c53db21787a47e2608` with 520 Hourly candles, 22 Daily
buckets and 149 fills. `c98e375` displayed all 520/22 candles at the first cursor;
the maintainer explicitly rejected that behavior because the K-lines did not
visibly replay. The older `visibleWindow()` behavior is also unacceptable because
it drops reached candles after 500 frames.
**Modify:** `Src/Bindings/Replay/ResultReplay.cpp` and its public header only if
a separate full-timeline read seam is required; `Src/Frontend/Replay/ReplayTab.cpp`,
chart/view interfaces as needed, focused Bindings/Frontend tests and owning specs.
**Interfaces:** immutable saved selected spans → complete indexed Hourly/Daily
timeline; replay cursor → untruncated progressive candle/fill/portfolio/trade-log
presentation.

- [x] RED: opening a saved result at its first cursor shows exactly one Hourly
  candle and no future fills. Step/seek/play grow the candle prefix to
  `currentIndex + 1`; Back/rewind shrink it and hide future fills. Add at least a
  multi-day fixture so Daily acceptance cannot pass with one candle by construction.
- [x] RED: prove there is no sliding 500-frame cap: cursor 499 presents 500
  candles, cursor 500 presents 501, and the final cursor presents every selected
  candle. No reached candle may disappear merely because playback advanced.
- [x] Prove Hourly↔Daily switching preserves the exact saved range and maps the
  current replay date deterministically. Each timeframe presents only its
  `0..cursor` prefix, clears stale markers and never moves a fill to a different
  source/aggregate bar. Back/seek hide both future candles and future fills;
  replaying restores them exactly once. Portfolio and trade log remain at cursor.
- [x] Use the complete immutable `timeline()` for indexed navigation and render
  `timeline().first(currentIndex + 1)`. Do not use the 500-frame
  `visibleWindow()` as chart semantics, rerun Engine, or read outside persisted
  selection spans.
- [x] For long histories, measure normal desktop responsiveness and use
  batched chart updates, virtualization or level-of-detail if needed. Every
  reached candle must remain navigable and the terminal frame must display the
  full selected range; future candles remain hidden until reached. Record the
  measured 23,777-candle terminal/seek behavior.
- [x] Operate the actual app with a multi-day, multi-fill saved result in both
  timeframes. Record total selected bars, Hourly candles, Daily buckets, initial
  candle/marker counts, intermediate and final candle/marker counts,
  Step/Back/seek/play/speed behavior and a screenshot at normal size. The
  one-day screenshot and the all-candles-first `c98e375` run are retained as
  failed product acceptance evidence.

**Accepted evidence:** the corrected frontend tests first failed in five places
against `c98e375`, then the focused Result Replay/chart suites passed 2/2. At
`05c23eb`, RunTest passed 220/220 in 61.73s and full non-fast RunQuality passed;
the coverage run passed 220/220 in 3608.61s with 199/199 changed lines and 41/44
changed branches. Earlier full-quality attempts failed, respectively, the
repository clang-format check, missing pinned `VCPKG_ROOT`, and a clang-tidy
ownership-transfer diagnostic; none is relabelled as passing.

The actual app opened Selectable Conditions result
`fdf8b59ca7681dfc575a744a5ae40ce9` with 520 Hourly candles, 22 Daily buckets
and 149 fills. Hourly visibly progressed 1/520 with no marker, 2/520 with one
Buy, and 13/520 with two Buy/two Sell markers and four synchronized rows. Play
advanced to 7/520, Pause stopped it, and rewind restored 1/520. Switching at the
first UTC date showed 1/22 Daily candles; Step showed 2/22 and Back returned
1/22. Normal-size screenshots were inspected during the desktop session but no
repository screenshot artifact was created. The separate 23,777-candle terminal
batch rendered every candle in 3.606s on this machine; this proves no truncation,
not instant rendering.

```bash
cmake --build --preset qt-dev --parallel
ctest --test-dir Output/qt-dev -R 'ResultReplayTest\.|bte_result_replay_frontend_tests' --output-on-failure
```

### Task 22 — Anchor Buy/Sell glyphs beside their K bars

**Status:** Accepted at `6f6fc86`. Task 9 proved opposite triangle directions
and deterministic overlap, but its tests accepted glyphs
centered at the fill price. That does not prove the glyph is visually associated
with the correct candle, especially across large ranges or Daily aggregation.
**Modify:** `Src/Frontend/Charts/QtChartsCandlestickView.cpp`, focused chart and
Result Replay frontend tests, `Docs/Specs/FrontendQt.md`.
**Interfaces:** exact immutable fill timestamp/price and containing candle →
candle-relative presentation glyph; stored values and synchronized log remain
unchanged.

- [x] RED with several non-uniform candles: every Buy glyph shares the containing
  candle's x-coordinate and is visibly below that candle's low; every Sell glyph
  shares the containing candle's x-coordinate and is visibly above that candle's
  high. Assert pixel geometry, not marker count or color alone.
- [x] Cover Hourly exact timestamps, Daily `candleTimestamp`, multiple fills on
  one candle, zoom/resize and progressive prefix replacement. A marker must never
  drift to the chart center, another bar, or a future hidden bar.
- [x] Keep each fill price in the marker series at its containing candle's x
  coordinate, and exact fill timestamp/price in persisted results and trade rows.
  At this historical checkpoint each fill had its own tooltip; Task 28 later
  groups Buy glyphs and retains the first reached fill tooltip plus the count.
  Presentation-only displacement may use a short leader;
  it must not rewrite persisted/accounting values.
- [x] GREEN focused suites, RunTest and full quality gate. In the actual app,
  inspect a multi-day multi-fill result in Hourly and Daily while stepping,
  playing, pausing and switching timeframe; record counts and screenshots.

Acceptance evidence: focused chart + Result Replay suites passed 2/2 in 9.55s;
RunTest passed 220/220 in 62.14s. On exact implementation revision `6f6fc86`,
the approved-network non-fast RunQuality invocation passed 220/220 coverage
tests in 62.84s, changed lines 260/260 and changed branches 41/44, plus
clang-tidy, cppcheck, IWYU and scan-build. The first quality attempt exposed a
false-positive CPP013 audit on a structured binding and the next sandboxed
attempt could not resolve the GoogleTest host; both were corrected/retried
rather than reported as passes.

Desktop result `783ed8ed88b0018fbb1df227b92480b2` contained 520 Hourly
bars, 22 Daily buckets and 149 fills. Hourly inspection at the 108-bar prefix
showed every visible Buy below its containing candle low and every Sell above
its high. Daily Step advanced 1/22 to 2/22; Play progressed to 11/22 and Pause
stopped it, with separated same-candle glyphs and synchronized rows. No
repository screenshot artifact was saved. This fixture deliberately cycles
`open = 100 + index % 7`, so similar-looking candles are expected and do not
validate real-market visual fidelity. Subsequent end-to-end acceptance must use
the varied OHLC in `StockData/Extracted` or another explicitly non-repeating
source; synthetic stress evidence remains limited to counts, playback and
marker geometry.

```bash
ctest --test-dir Output/qt-dev -R 'bte_frontend_chart_tests|bte_result_replay_frontend_tests' --output-on-failure
```

### Task 23 / R12 — Prevent wheel zoom from hiding all Replay candles

**Status:** Repair committed at `cf4d85b`; exact `15dfcab` full gates pass,
but final gesture/device and review acceptance is pending. On an actual saved AAPL Replay at 10/48,
one desktop scroll expanded price/volume axes to enormous ranges and left the
K bars effectively invisible while the Replay cursor and records remained.

- [x] Reproduce the user-visible failure in the app with real varied OHLC.
- [x] Add a failing Qt chart regression: 40 small wheel deltas and 40 full
  events in one burst must keep hourly candle time and price ranges readable.
  The full-event assertion failed before the burst limit was added.
- [x] Normalize angle/pixel wheel displacement and cap one continuous gesture
  to two wheel notches, without changing Replay data or persisted results.
- [x] Rebuild the app and repeat the same desktop scroll at 9/48: the visible
  candles remained after zoom in and reverse zoom, with the Buy marker present.
  Focused chart and Backtest frontend suites passed 2/2.
- [x] Review found a Reset Zoom boundary: immediate same-direction scrolling
  was ignored after burst saturation. A new test failed on that behavior;
  clearing gesture state on reset and bar-window replacement made it pass.
  A separate Qt ScrollBegin test confirms a later gesture can zoom again.
- [ ] Finish final diff review and check normal mouse/trackpad behavior and
  limits after a later independent desktop scroll gesture. Exact `15dfcab`
  `./RunTest.sh` and full non-fast `./RunQuality.sh` passed 232/232, changed
  lines 832/845 = 98% and changed branches 103/108 = 95.4%. A saved AAPL
  Replay at 11/48 kept visible candles after a wheel-up event; one event does
  not verify all devices or repeated independent gestures. The wheel cap is
  per Qt gesture, or a 750 ms inactivity gap for unphased wheel events; it is
  not a universal gesture-recognition guarantee across every input device.

This is a chart interaction regression, not evidence that long-range source
selection (Task 13/R8) or every Replay zoom/pan scenario is accepted.

### Task 24 / R13 — Correct K-line time, price and volume presentation

**Status:** Implementation and focused desktop verification in progress on
2026-10-06; final acceptance remains open. The maintainer's example is a presentation reference, not a
source of market data or authority to copy third-party branding. The saved
AAPL result `7e1607c01fa2da46cd797c6edb883ba2` has 23,777 selected
Hourly bars and 103 persisted fill records (independently counted by querying
the result's `canonical_records` family 1). An earlier checkpoint incorrectly
said 108; that count is superseded here. Results opened that exact ID; Replay began at
1/23,777. Its screenshot shows a single enormous red candle and a volume line
overlaid in the price plot, with repeated `05-01` x labels for one day. This is
not an acceptable hourly/daily K-line chart even though the underlying bars
and fills are present.

**Modify:** `Src/Frontend/Charts/QtChartsCandlestickView.cpp`, its public
header only if needed, `Src/Frontend/Replay/ReplayTab.cpp`, focused chart and
Replay frontend tests, `Docs/Specs/FrontendQt.md` and this plan.
**Boundary:** visualization only. Preserve immutable selected bars, OHLCV,
fill timestamps/prices, chart-to-result ID and canonical hash.

- [ ] RED: each actual selected Hourly bar is one discrete candle at its UTC
  hour; each Daily UTC bucket is one candle at its date. Assert mapped x
  centers are ordered and distinct, including 1/2 bars, missing-hour gaps,
  a UTC day transition and a long-window tail. A sparse one-bar prefix must
  remain a normal-width candle, not a plot-wide body.
- [ ] RED: distinct aligned price and volume panels. OHLC wicks/bodies and
  Buy/Sell glyphs stay in the upper price pane, colored per-candle volume bars
  stay below; price y scale cannot be set by volume and volume y scale cannot
  distort price. No overlay line/secondary volume axis in the price pane.
- [ ] RED: x ticks communicate Hourly date **and hour** and Daily date with
  UTC context, avoiding identical labels at consecutive hourly ticks. Price
  y ticks show quote price (current USD), volume y ticks show shares; extremes,
  flat bars, high volume, zoom, pan, resize and timeframe switching stay
  readable and synchronized.
- [ ] GREEN: preserve progressive 1→N playback and exact-time markers. Check
  chart geometry/axes in automated tests and inspect actual multi-fill AAPL
  Hourly and Daily at initial, intermediate and terminal positions, including
  one wheel gesture. Compare visible OHLCV and markers with saved data; record
  screenshots and measured counts. Full exact-HEAD gates and final review
  follow before a PR; CI pass alone cannot substitute for this desktop check.

**2026-10-06 checkpoint, not acceptance:** The price pane is now separate from
an aligned colored per-bar volume pane; the price axis has no volume overlay.
Candles have a 12-pixel maximum column width, and the lower pane shares the
upper pane's time range and x geometry. Hourly labels contain date/hour and
Daily labels date; the lower time axis declares UTC. Focused chart tests were
RED before the implementation and now pass (20/20), including a same-time
pixel-alignment assertion, one-bar width, pane separation and Daily zoom.
`./RunTest.sh` passed 236/236 on the dirty working tree, **not** yet on a
committed HEAD. In the rebuilt app, saved AAPL result
`7e1607c01fa2da46cd797c6edb883ba2` displayed one initial Hourly candle,
251/23,777 Hourly candles after seek with a Buy glyph beside its containing
candle, and 17 Daily candles after timeframe switch with corresponding volume.
The initial chart revealed width and pane-alignment defects, both corrected and
retested. Exact committed `c873211` then passed `./RunTest.sh` (236/236) and the
full non-fast `./RunQuality.sh` (clang-tidy, cppcheck, IWYU, scan-build,
236/236 coverage tests, 98% changed-line and 94.7% changed-branch coverage).
The final-build desktop check showed 251/23,777 Hourly candles and aligned
volume, then all 23,777 at the terminal cursor without truncation; switching
to Daily gave 1,522 total bars and a 293-bar intermediate view with several
Buy/Sell markers on their containing daily candles. A wheel gesture on the
full Hourly range retained candles and volume; a Daily horizontal drag moved
price candles, trade glyphs and volume bars together. The first accessibility-tree
capture at the 23,777-bar terminal cursor timed out, but a direct screenshot
showed the app remained responsive; a subsequent wheel action and tree capture
also succeeded. A new RED test found scientific-notation share ticks and the
chart now formats whole-share labels; the restarted `4d8e482` desktop app
showed numeric share ticks (0, 2,419,982 and 4,839,965 at 251/23,777).
Exact committed `4d8e482` also passed `./RunTest.sh` 236/236 and the full
non-fast `./RunQuality.sh` (236/236 coverage tests, 98% changed-line and
94.7% changed-branch coverage). Two later automated checks verify literal
OHLCV/timestamp preservation through the chart adapter and aligned price/volume
x coordinates at widths 620, 900 and 1100; the focused suite passes 22/22.
Still open: actual saved-result data-by-data OHLCV/fill comparison, explicit
resize desktop inspection, full exact-HEAD gates after any later edits,
final review and the broader plan tasks.
The pre-correction working tree passed `./RunTest.sh` 236/236; that was not
exact-commit evidence. Exact committed `3245574` later passed the complete
`./RunQuality.sh` after the header/RAII corrections, including 236/236 tests,
98% changed-line and 94.7% changed-branch coverage. This is not a claim that
Task 24's desktop data comparison is done.
The first `6994ae7` full-quality attempt stopped at the chart's
`readability-qualified-auto` lint finding on an axis pointer. The declaration
was corrected in the next checkpoint; that failed attempt is not a pass.
The next exact-HEAD `bbbf3ec` full-quality attempt passed clang-tidy/cppcheck
but stopped at IWYU: the new chart needed a direct `QAbstractAxis` include and
had one redundant graphics include. Focused IWYU passed after correction;
the complete pipeline passed on `c873211`, but the later label edit requires a
new exact-HEAD run.

**2026-10-08 follow-up, not acceptance:** The dirty-worktree `./RunTest.sh`
passed 240/240 after changing the chart's actual price/volume plot ratio,
removing unwanted chart scrollbars, and suppressing colliding UTC labels.
In the isolated rebuilt macOS app, a named 48-hour AAPL Result opened by its
exact ID; Hourly showed distinct OHLC candles, aligned volume and a Buy marker.
Mouse-wheel zoom retained the candles. Switching to Daily (UTC) through the
actual selector showed one candle per reached UTC day; stepping progressed
from 1/3 to 3/3 rather than revealing all three at once. This short Result
does not satisfy Task 24's long-range multi-fill desktop comparison, nor does
it substitute for final intended-`Launch.sh` and exact-HEAD gates.

**2026-10-08 newly observed usability defect, not fixed:** In an isolated
desktop run of a 2018-05-01–2024-05-16 AAPL Selectable Conditions Backtest,
23,777 hourly bars produced 5,261 persisted fills. Progressive Replay worked,
but candles remained too thin for the available 2880-pixel-wide window.
`QtChartsCandlestickView` still caps candle columns at 12 pixels, and the
viewport defaults to 14-pixel slots at that historical checkpoint. The
maintainer requested wider, more legible candles. At that checkpoint the
`Follow latest` action was connected and
returns after horizontal review, but zoom can leave the newest candle outside
the visible window while the viewport still reports following mode; the UI
then disables the button until another frame or pan. A focused RED/GREEN
regression, updated approved width contract, three-size desktop inspection,
and the long-range data-by-data comparison remain open. Do not claim this
visual task accepted on the basis of the existing passing quality gate.

**2026-10-09 repair checkpoint, not final acceptance:** Three focused
regressions first failed against the old behavior: 14-pixel slots exposed too
many thin candles at the 23,777-bar tail; a rendered one-bar body measured
only 2 pixels; and zoom moved the newest bar offscreen while Follow latest
remained disabled. The implementation now defaults to 24-pixel slots, bounds
the default rendered body to roughly 16–20 pixels, lets body width increase
with zoom, and enables Follow latest whenever the newest reached bar is not
visible. The three affected Qt suites passed after the fix. The dirty-tree
`./RunTest.sh` then passed 241/241 on 2026-10-09; this is not exact-HEAD
evidence. In the isolated rebuilt app, saved result
`90e0f8745fd5195a67fb1c8647892880` opened at its first Hourly candle and
then reached 23,777/23,777 with separate price/volume panes and multiple
Buy/Sell glyphs beside the recent candles. Saved result
`efa9d65fbb1af26024ee6e9a0132dea7` reached 47/48; five Zoom In clicks
made Follow latest enabled, and pressing it moved the viewport to the newest
reached candle and disabled the button. A 23,777-bar direct chart test measured
3,632 ms in an offscreen Qt run. The desktop seek to that terminal cursor
temporarily made UI observation time out, then recovered; its end-to-end
latency and cause are not yet measured. Full-screen resize initially left a
large unused region at the right of the chart until Reset zoom refreshed it.
These are open performance/resize findings, not accepted behavior. Exact-HEAD
RunQuality, three-size actual-app interactions, and saved-result OHLCV/fill
comparison remain open.

**2026-10-09 review follow-up, not final acceptance:** The independent code,
documentation and test axes found no proven new production defect in the width
or Follow latest repair, but found two P2 test gaps and two P2 documentation
gaps. The Follow latest test now checks that returning to the tail preserves
the zoomed visible-slot count. Rendered-body assertions now cover 1 and 200
bars at widths 1024, 1200 and 1440, plus the 23,777-bar tail. A 23,777-bar
offscreen chart load measured 3,654 ms and adding 5,261 marker glyphs measured
751 ms; these are chart-only figures, not the end-to-end desktop seek latency.
The two affected Qt suites passed after the extra assertions. The separate
viewport plan's old 12–16-pixel target and stale status, this plan's old
headline/review point, and the DataLayer launcher wording were corrected.

**Earlier Task 19 plan-only consolidation (historical):** added Task 19, updated screenshot-known
facts and cross-task coverage; no launcher, app, Engine or test code changed.
Existing uncommitted source/build/tests remain preserved. Self-review and
`git diff --check` are document checks, not new implementation acceptance.

**2026-10-10 performance and review checkpoint, not final acceptance:** A native
23,777-bar seek stalled while Qt rebuilt hidden volume-line geometry for every
appended point; a read-only process sample identified that production stack.
The batch-volume regression failed with 200 per-point notifications and now
passes with zero point-added notifications and one points-replaced notification.
Volume values and count remain exact. A second sample identified costly native
accessibility work during per-cell trade-table notifications. Trade rows now
grow in one insertion and cells emit one correctly bounded data-change range;
the regression first observed 18 notifications instead of one. Coverage checks
the insertion range, changed-cell range and final new row's accessible metadata.
A real persisted-result integration test builds 23,777 bars and 6,793 fills,
opens Replay, seeks to the terminal candle, verifies counts and first/last fill
times, then verifies Follow latest preserves zoom span, cursor and exact Result
ID, and rewind clears future fills. The latest focused run passed 4 Qt test
results in 8.650 seconds; its terminal seek measured 2,391 ms offscreen.
These are working-tree checks, not exact-HEAD gates. In the freshly rebuilt
isolated native app, dragging result `90e0f8745fd5195a67fb1c8647892880` to
23,777/23,777 and reading accessibility state took 3,321 ms including automation
overhead. The inspected screenshot shows varied OHLC bodies, aligned volume
columns and Buy/Sell triangles beside their candles. Later inspection did
reproduce fullscreen shared-axis reentry and a rendered time-grid mismatch,
despite matching reported plot rectangles. These supersede the earlier
non-reproduction observation. Queued current-geometry updates avoid reentry;
automatic layout with shared left insets aligns axes, grids and series without
manual plot-area overrides. A pixel regression failed at the expected grid
column (329) while the old grid appeared at 290; the automatic-margin repair
passes. An extreme-volume bar at the visible boundary then exposed continuing
layout oscillation (20 changes in 20 further event-loop turns). Retaining the
largest inset within a timeline stabilizes it, and resetting insets on timeline
replacement restores ordinary-data geometry; both regressions now pass.
The latest two focused cases passed 4 Qt test results in 318 ms. This is not
full-suite or native-desktop acceptance. A subsequent native launch initially
reported loading status through accessibility while sampled processes were
idle. Coordinate interaction and fresh screenshots subsequently confirmed a
successful load and terminal seek. Native Zoom in → Follow latest → fullscreen
then preserved 23,777/23,777, candle bodies, adjacent fill triangles, and aligned
price/volume grids without the earlier stall. This observation used the
automatic retained-inset build before the later timeline-reset repair; it is
not final exact-build acceptance. Three-size, while-playing
and splitter desktop acceptance, complete intermediate-fill comparison and
exact committed full gates remain open.

The subsequent dirty-tree `./RunTest.sh` passed 241/241 in 83.85 seconds.
Formatting and later test-strengthening edits require rerunning that gate;
this does not replace exact committed-HEAD test/quality evidence or CI.

The 2026-10-11 rerun failed two Qt tests (239/241 test executables passed):
the Follow latest test compared rounded visible-bar counts rather than candle
density, and the volume-layout test could accept an unchanged pre-layout
rectangle before the requested margin change took effect. The focused tests
now wait for actual layout delivery and verify the preserved pixel-per-slot
density instead of an incidental bar-count rounding boundary. Both focused
cases passed (3 Qt results each). The full suite is being rerun; no new
exact-HEAD quality or desktop acceptance is claimed.

### Task 25 — Human-readable saved Result names

**Status:** Implemented in the current worktree, not accepted. The full
`./RunTest.sh` suite passed 241/241 on committed `12fe8eb` on 2026-10-08,
and the isolated desktop save → Results → Rename → Open Replay workflow
confirmed the same immutable Result ID with its updated display name in the
Replay selector. Exact-HEAD `./RunQuality.sh --base
84e3a135b18bdddb8134713df0b6896be0cc1a37 --head HEAD` passed on
`12fe8eb`: 241/241 tests, 98% changed-line coverage, and 94.0% changed-branch
coverage. The offline GoogleTest fetch used a process-local URL substitution
pointing to the already cached pinned v1.14.0 Git checkout; no dependency or
gate was changed. Final review, CI, and whole-plan prerequisites remain open;
this is not a complete-plan acceptance claim.
The first exact-HEAD full-quality attempt on `b95b344` failed at clang-tidy
on the new UTF-8 validator and Backtest publisher callback. Those findings
were corrected at `65c184f`, with focused clang-tidy passing. The full-quality
attempt on `65c184f` passed clang-tidy and cppcheck but stopped at IWYU on
three missing display-name initializers and direct Qt includes. The fixes
landed in `7756f04`; targeted negative/boundary naming and corrupt metadata
coverage landed in `12fe8eb`, after which the exact-HEAD full gate passed.
The existing Results and Replay selectors expose opaque Result IDs, making
multiple saved runs hard to distinguish. A display name is presentation
metadata, not a Result ID, strategy identity, canonical record, or hash input.

**Contract:** Offer an optional English-labelled Result name field at Save Result and
Save & Replay. The name persists with the Result and appears in the Results
catalog, details, and Replay selector, while the immutable Result ID remains
available for disambiguation. Results should also allow renaming a saved
Result without rerunning Engine. Existing unnamed artifacts fall back to a
useful ID-derived label and remain readable. Name changes must preserve
canonical hashes, fills, bars, Trash/Restore identity, and result ordering.
Trim whitespace, reject overlong/control-character supplied names and empty
rename attempts, and display
clear validation/storage errors without a false success message.

- [x] RED: save-time naming, both Save actions, repeated save, unnamed legacy
  Result fallback, duplicate names with distinct IDs, and no hash change.
- [x] RED: Results rename, open exact selected ID after renaming, and
  Trash/Restore retaining the name.
- [x] GREEN: update ResultStore/bindings/Backtest/Results/Replay and owning
  specs; perform keyboard and actual desktop naming workflow in isolation.
- [ ] Run all affected tests, exact-HEAD full quality, final review, and CI
  before claiming this task or the whole Issue 10 plan complete.

### Task 26 — Trade-marker clearance and maximum-playback readability

**Status:** Marker-clearance and compression repairs have exact committed
full-gate evidence through `d209d21`, plus isolated AAPL Max endpoint and
zoom/Follow checks. The overall task is not accepted: complete resize/layout
and responsiveness evidence remains open. The user
reports that Buy/Sell triangles feel attached to candles and that the final
display becomes compressed after using "max". Confirm whether "max" means
the playback speed or window maximization before attributing a cause.

- [x] Confirm the bounded presentation change: at least 8 logical pixels
  of visible clearance between a triangle and its candle's low/high (eight
  pixels to the 16-pixel glyph box, approximately nine/ten to the triangle tip),
  independent of zoom. Preserve the containing candle's x slot, persisted
  price/time, and financial records. Keep edge markers legible rather than
  clamping them back into a candle.
  Confirmed by the maintainer on 2026-10-12. Marker-clearance implementation now
  has committed full-gate evidence at `9ae0f5e` and `d209d21` below; this is
  distinct from complete native layout acceptance.
- [ ] Reproduce the reported compression through the actual app and record
  its launch revision, selected Result ID, timeframe, density and follow mode.
- [ ] Add a regression at the real playback/chart seam for maximum speed
  reaching the final frame without fitting the entire timeline or changing
  selected density. Test maximization separately if that is the reported path.
- [ ] Verify marker clearance, coincident fills, plot boundaries, Hourly/Daily,
  zoom/pan/Follow latest, and final-frame playback in focused tests and native UI.
- [ ] Pass full exact-HEAD gates and review before accepting either repair.

**Current resize-repair evidence:** Strengthened boundary activation assertions
exposed another reload-layout failure after the earlier 241/241 run. Activating
both automatic layouts before measuring their intrinsic insets made the
strengthened extreme-volume/ordinary-reload test pass (3 Qt results, 274 ms).
After the two layout-sensitive test corrections above, the current production
change passed `./RunTest.sh` (241/241, 80.58 seconds). Native testing still
reproduced a material failure: the old running build had collapsed price and
volume to a narrow right-edge strip at 7,528/23,777, while Max was selected
but playback was paused. A fresh latest-build launch opened Result
`90e0f8745fd5195a67fb1c8647892880` in Hourly mode and started actual Max
playback from the first candle. During playback its shared left inset grew
visibly (from approximately 216 to 720 physical screenshot pixels), reducing
usable chart width despite individually readable candle bodies. The capture
also took 75 seconds, indicating an interaction-responsiveness problem.
Playback was paused at 20,962/23,777. The full suite did not detect this
progressive failure; a new regression is being developed. This slice remains
unaccepted, with exact-HEAD quality, final native acceptance, marker-clearance
implementation and whole-plan completion still open. No PR has been opened.

The new progressive-label regression reproduced the same accumulating-inset
failure at candle 89: the price plot's left edge grew from 78.7969 to 179.563
logical pixels with only ordinary three-digit prices and seven-digit volumes.
The repair now retains the largest intrinsic label requirement separately from
compensated, rounded chart margins. The complete chart suite passed 40/40 Qt
results in 9.939 seconds, including the boundary-volume, timeline-reload,
rendered-grid and reentry regressions. Debug instrumentation was removed.
Full tests and latest-build native Max playback are being rerun; this focused
evidence does not accept the slice or the whole plan.

The subsequent full dirty-tree run passed 241/241 in 89.55 seconds. Review
requested a tighter progressive-regression bound; it now permits at most
20 logical pixels of extra label space for the fixed fixture and checks that
both plots stop changing over 20 further event-loop turns after settling.
The strengthened focused case passed 3 Qt results in 5.962 seconds, and the
latest `./RunTest.sh` passed 241/241 in 87.86 seconds. Three independent
read-only review axes found no remaining P0–P2 defect in this checkpoint after
closing the consolidated inventory/headline status gap; the tighter regression
also closes the test-strength suggestion. Working-tree standards and touched
C++ formatting checks passed. This is a recoverable implementation checkpoint,
not an accepted complete plan: latest native playback, exact committed full
quality, marker clearance and remaining plan requirements are still open.

**Exact-checkpoint follow-up (2026-10-11):** `01620b9` passed `./RunTest.sh`
(241/241, 88.57 seconds). Its full non-fast RunQuality passed format,
standards, clang-tidy and cppcheck, then failed IWYU: `ReplayTab.cpp` required
a direct `ReplayBarViewport.h` include and removal of the redundant
`QSignalBlocker` forwarding include under the checked-in Qt mapping. A first
focused IWYU diagnostic used a relative mapping path that was unavailable
from the compiler's working directory; it is not authoritative. Repeating
with the absolute checked-in mapping reproduced exactly the two required
include corrections. Scan-build and coverage were not reached in this run.
The correction is behavior-preserving; no new RED behavior test is applicable.
Full gates must be repeated on the corrected commit.

The isolated latest-build macOS app opened Result
`90e0f8745fd5195a67fb1c8647892880` and completed actual Hourly Max playback
from its first candle to 23,777/23,777. At completion its price plot's left
edge remained approximately 222 physical screenshot pixels, rather than
growing to the previous narrow right-edge strip. Zoom In, Follow latest,
window fullscreen, wheel zoom and Follow latest were exercised: candles
remained visible and the newest date returned without changing the playback
cursor. Daily switching completed at 1,522/1,522 with the same final date
(2024-05-16) and unchanged portfolio totals. These Daily bars are UTC
aggregations, not verified official session closes for Tasks 17/18.

An accessibility observation during Max playback timed out; a subsequent
read-only process sample showed the main thread waiting in the normal event
loop, and later completion observations returned promptly. This does not
establish a measured responsiveness budget or prove every playback
interaction is responsive. After restarting the same build, ordinary 1x
playback was exercised while resizing from 1,440x928 to 1,200x800 and
1,024x768 logical pixels. Candles retained readable body widths and aligned
price/volume columns; smaller windows required vertical scrolling to see
the complete volume region. Pause was exercised. Splitter interaction,
complete three-size acceptance, marker clearance and whole-plan acceptance
remain open. No PR has been opened.

**Corrected exact-HEAD gates (2026-10-11):** `ffbb512` passed the separate
`./RunTest.sh` (241/241, 93.28 seconds) and full non-fast
`./RunQuality.sh --base 84e3a135b18bdddb8134713df0b6896be0cc1a37 --head HEAD`
(exit 0). Format, project standards, clang-tidy, cppcheck, IWYU and scan-build
passed; scan-build reported no bugs. The coverage suite passed 241/241 in
95.98 seconds, with 2,037/2,077 changed lines covered (98% displayed) and
229/244 changed branches (93.9%). Non-user-code Qt analysis warnings and
dependency build warnings were present; they were not waived findings in
project code. The complete gate replaced the earlier failed IWYU run rather
than skipping it. This evidence covers that committed checkpoint, not these
later ledger edits or the outstanding plan requirements.

A separate read-only source/count audit matched the same saved result's
selected interval `[2018-05-01T00:00Z,2024-05-17T00:00Z)` against
`StockData/Extracted/AAPL.csv`: 23,777 source rows and 1,522 distinct UTC date
buckets. The saved data spans total 23,777 rows, beginning at
2018-05-01T08:00Z and ending at 2024-05-16T23:00Z; its portfolio checkpoint
count is 23,777 and fill count is 5,261. Those counts and endpoints agree with
the observed Hourly/Daily terminal UI. This is independent count/range
evidence, not a claim that every rendered candle was manually compared.
The initial database diagnostic used a nonexistent `Active` directory and
failed to open; the subsequent successful inspection resolved the real
`Results` path first and used `sqlite3 -readonly`.

Further ordinary-playback UI observations confirmed Pause retained cursor
33 across subsequent checks, then Resume advanced through 35 and 65. The
attempted chart/details splitter drag did not visibly change the chart
height; it is not accepted splitter evidence. In smaller windows, volume
and details exceeded the visible viewport; scrolling access remains to be
verified rather than assumed. Task 26's marker tip-to-wick change is a
separate bounded design confirmed by the maintainer on 2026-10-12; its
RED/GREEN implementation and acceptance are tracked below. Official daily/session history for Tasks 17/18 remains
blocked on a specified trusted local file or entitled data source. No PR,
push or merge has been performed.

**Marker-clearance checkpoint (2026-10-12, working tree only):** The strengthened
glyph-bounds assertion failed under the previous 12-pixel center offset: the
triangle box did not leave eight logical pixels after the wick. A 16-pixel
center offset passed that regression, but removing vertical clamping exposed
clipping and failed the existing coincident-fill test. That intermediate
implementation was not accepted. Fixed visual edge padding derived from visible
OHLC and plot height now preserves the gap without including fill prices or
volume in autoscale. A public height-only resize regression with a valid
near-zero low then failed; observing plot height as well as width repaired it.
The focused three-case run passed 5 Qt results in 216 ms, and the complete
chart suite passed 41 Qt results in 9.848 seconds. Full tests, exact committed
quality, independent review and native latest-build acceptance remain open.
An initial boundary-test attempt used an invalid zero-low Bar; a separate
manual-axis probe was replaced with public resize/timeframe interactions.
One probe also ran an older binary after a test compilation error; it is not
acceptance evidence. No production diagnostic instrumentation was added.

The ordinary-gap assertion now also enforces an upper bound of nine logical
pixels, preventing an excessively distant marker from passing. A new regression
first failed because single fills still displayed connecting lines; single fills
now have no leader, while coincident same-side fills retain their fan leaders.
The two-fill boundary checks two visible glyphs and zero leaders, then three
visible glyphs and two leaders after adding another Buy, and all three counts
return to zero after clearing. The chart suite passed 42 Qt results in 9.884
seconds before its activation assertions were strengthened; the latest focused
two-case run passed 4 Qt results in 187 ms. Three independent read-only review
axes found no remaining P0–P2 finding in this marker delta after the documented
status and test-boundary corrections. This is not whole-branch acceptance.

The isolated latest-production macOS app again opened the same AAPL result.
Hourly terminal seeking, wheel zoom and Follow latest retained the candles and
latest date. Daily Max playback was actually started from the first candle and
completed at slider index 1,521 (1,522 reached UTC-day bars), with readable candle
widths, aligned volume and unchanged final portfolio totals. Ordinary single
fill glyphs visibly leave a wick gap without a leader. Dense neighboring
coincident-fill fans can still overlap visually; this observation does not
establish complete overlap or three-window-size acceptance. Exact committed
full gates and the other outstanding plan requirements remain open.

The latest working-tree `./RunTest.sh` passed 241/241 in 90.30 seconds,
including the strengthened activation assertions (complete chart executable:
10.29 seconds). This precedes the checkpoint commit and is interim evidence;
the required separate full test and quality runs must cover its committed HEAD.

**Exact checkpoint follow-up:** `8879169` passed the separate `./RunTest.sh`
(241/241, 90.42 seconds). The full non-fast RunQuality passed format, standards,
clang-tidy and cppcheck, then failed IWYU: libc++'s overloaded `std::abs`
requested a C `stdlib.h` include for the new floating-point size comparisons.
An initial conversational diagnosis incorrectly suspected `initializer_list`
from truncated output; the complete focused diagnostic corrected that claim.
Using the equivalent explicitly floating-point `std::fabs` from the already
included `<cmath>` passed focused IWYU without adding a C header or suppression.
This preserves the size-change behavior; no new RED behavior test is applicable.
The rebuilt three-case marker regression run passed 5 Qt results in 205 ms.
Scan-build and coverage were not reached. Full gates must cover the corrected
committed revision before this slice can be accepted.

**Corrected exact-HEAD gates:** `9ae0f5e` passed the separate `./RunTest.sh`
(241/241, 89.91 seconds) and full non-fast RunQuality (exit 0). Format,
standards, clang-tidy, cppcheck, IWYU and scan-build completed successfully.
The coverage suite passed 241/241 in 96.00 seconds; 2,045/2,085 changed lines
were covered (98% displayed), with 229/244 changed branches (93.9%). This does
not cover later ledger edits, the newly requested one-Buy-glyph grouping, or
whole-plan acceptance. Separate sanitizer and final review requirements remain
open for the complete submission.

Further native observations at 1,200x800 and 1,024x768 confirmed that scrolling
outside the chart reaches the volume axis, portfolio and trade log. Scrolling
inside the price plot instead zooms, as designed. A splitter drag still did not
produce a verified height change. At the smaller size, coordinate-based Zoom In
and Follow latest retained terminal Daily candles and slider index 1,521.
An earlier accessibility-index interaction unexpectedly restarted playback and
reset speed; its intended zoom/follow result is not accepted evidence.

In the isolated test store, Result `90e0f8745fd5195a67fb1c8647892880` was renamed
to `AAPL six-year replay QA`, displayed in the catalog and details, then renamed
back to `AAPL six-year 0.2% condition run`. Results Open Replay selected that
exact ID, retained its display name and showed its first UTC Daily candle.
The earlier authorized Move to Trash was confirmed for this exact test Result;
Saved Results then contained only the untouched `Test 2` result. Subsequent
mouse/keyboard attempts to select Trash did not change the location. Rebinding
and resetting the computer-use connection did not establish a successful
Restore; one attempt explicitly reported inactive computer use. The maintainer
was asked to confirm the desktop is unlocked/available. The AAPL test Result
was recoverable in Trash, not permanently deleted. The later recovery below
supersedes this interruption; no production result store was modified.

On 2026-10-12, the documented native window **Raise** action followed by keyboard
selection changed the location to **Trash**. The sole AAPL row was selected and
**Restore** invoked. Returning to **Saved Results** showed two results: the
untouched Test 2 and AAPL with its original name and exact ID
`90e0f8745fd5195a67fb1c8647892880`, Completed and Available. This verifies the
approved recoverable test-only cycle; it does not establish permanent-delete,
30-day expiration or whole-plan acceptance. The prior input interruption is
resolved, without proving that the desktop had been locked.

### Task 27 — Workspace and PR delivery inventory

**Status:** Requested on 2026-10-12 / in progress. Publish verified, scoped
changes and report any remaining code without erasing or duplicating local work.

- [x] Read current GitHub PR state: #68, #69 and #70 are merged. The latest
  remote main is `84e3a135`; no Issue 10 follow-up PR currently exists.
- [ ] Review all issue10 post-main checkpoint commits and current uncommitted
  changes; submit only after plan acceptance.
- [x] Reconcile seven locally ahead commits on
  `feature/issue-10-result-replay` with the hardening variants already merged
  through #70. Different commit/patch IDs alone do not establish missing code.
  `origin/main` and `codex/finish-issue10-plan` have identical `Src` and `Tests`
  trees. The seven fixes have corresponding rewritten commits in #70; remaining
  differences from the older local branch add hardening, direct includes,
  characterization and boundary tests. Publishing the older branch again would
  duplicate already merged work, not deliver missing code.
- [x] Review the root's untracked older plan copy and
  `Tests/Unit/Tools/UnitTest_DependencyManifest.py`. The worktree directory is
  a checkout/build container, not a file tree to stage into the repository.
  The older root plan is a superseded draft, not an overwrite candidate for
  this tracked ledger. The root test's intent is unique but its string dependency
  entry and override without `port-version` are stale relative to the accepted
  manifest. Ported the pin assertion into this worktree using the actual typed
  dependency `version>= 3.53.4`, the exact override `3.53.4`/port revision `0`,
  and the existing immutable baseline. Preserved both original root files.
  The focused manifest test passes 1/1. The registered complete Tools scope
  `ctest --preset qt-dev --no-tests=error -R '^bte_project_standards_tests$'
  passed 1/1 CTest entry in 34.87 seconds; this is not the whole-project suite.
  Exact committed `RunTest.sh` verification of the port is recorded below. Three
  independent review axes found no remaining blocker. This adds coverage of unchanged policy, not a behavior fix
  or an invented RED regression. Existing unittest discovery registers the
  new file without build-wiring changes. No new dependency/version was adopted.
  The subsequent exact committed `9bb2a9e` `./RunTest.sh` passed 241/241 in
  85.31 seconds, including the new discovered manifest test. A following
  documentation-only clarification records that scatter x values are candle
  slots, while exact fill timestamps remain in persisted results/trade rows.
  No C++/dependency/build behavior changed in this coverage slice; the latest
  complete non-fast C++ quality evidence remains `d209d21`. Repeat all required
  gates on the eventual PR submission HEAD; no CI/push is requested now.
- [x] Confirm `codex/implement-ci-slices` is the merged #69 head and classify
  local main's old documentation commit against that PR before any cleanup.
  The implementation-skill files and both related design/implementation plans
  are identical to `origin/main`; their changes are already merged. Local refs
  and the older checkout were preserved, not reset or deleted.
- [ ] Report each concern as merged, submitted/unmerged, unpushed, superseded,
  or blocked, with exact revisions and PR links. Do not claim all code shipped
  until the remote refs and PR state confirm it. Do not delete local backups,
  drafts or untracked files as an implied cleanup operation.

### Task 28 — One Buy glyph per displayed candle

**Status:** Scoped Buy-only checkpoint accepted at `d209d21` on 2026-10-12.
Whole-plan acceptance and delivery remain open.
The maintainer repeated the explicit Buy requirement while requesting continued
implementation. Sell behavior remains unchanged; grouping Sells is not inferred.
The maintainer wants
multiple Buys in one displayed K bar represented by one Buy triangle rather
than a fan of triangles. This supersedes the Buy fan presentation,
not the complete persisted fills or synchronized per-fill trade log.

- [x] Record the explicit Buy request. Group by the containing displayed candle:
  Hourly uses its hour bar; Daily uses its UTC date bucket. Do not group only by
  identical fill timestamps, or accidentally combine different candles.
- [x] Apply the bounded Buy-only presentation: one triangle per candle, retaining
  the first reached fill tooltip and adding the reached Buy count for multiple
  fills. The complete per-fill log is unchanged. Do not change Sell grouping.
- [x] Add a RED regression for appending a second Buy to the same candle: one
  visible Buy glyph and unchanged financial series points. The old code failed
  with three total glyphs instead of two (Buy plus Sell), and failed the
  ten-fill display regression. Both pass with grouping. The per-fill trade-log
  code is untouched; its integrated desktop comparison remains open below.
  Cover one/two/zero fills, opposite sides, different candles and invalid fills.
- [x] Focused chart tests cover Hourly/Daily, rewind/replacement, later same-candle fills and timeframe switching
  without exposing unreached fills in either glyphs or tooltip counts. Preserve
  fixed wick clearance and immutable financial records; use no fan leaders for
  grouped glyphs.
- [x] Update owning chart specs, pass focused and complete exact-HEAD gates,
  review, and inspect the actual app before accepting the grouped presentation.

Working-tree focused evidence: full `bte_frontend_chart_tests` passed 43 Qt test
results (including init/cleanup), zero failures, in 10.264 seconds. This is not
the final exact-HEAD gate. Existing ten-fill literal scatter-point assertions
still retain five Buy and five Sell financial points while drawing one Buy and
five Sell glyphs; Sell leaders remain unchanged. Complete tests/quality and
native grouped-glyph/complete-trade-row inspection are pending.

Review requested distinct-price/timestamp assertions and a persisted same-Daily
candle trade-row integration case. Added exact points `(0, 101)` and `(0, 102)`
with the first fill tooltip at `2024-01-02T01:00:00Z`, plus the existing real
snapshot-backed selectable Replay case: first UTC day keeps all seven ordered
trade rows (four Buys, three Sells), four exact Buy scatter prices `101.0101`,
and one Buy glyph with a reached count of four. The initial integration-test
build failed `-Wsign-conversion` on an array index; that was corrected with an
explicit checked-range index conversion. A subsequent run of the stale binary
was not accepted. After a successful rebuild, the focused integrated test passed
three Qt results (init/case/cleanup) in 0.675 seconds; the distinct-price chart
case passed three results in 0.153 seconds. Earlier working-tree `RunTest.sh`
passed 241/241 in 89.73 seconds before these strengthening assertions; rerun
on the exact committed checkpoint is still required.

**Exact checkpoint acceptance:** On committed
`d209d21890f1718efc2c584f88b6de3a665dfa5e`, separate `./RunTest.sh` passed
241/241 in 88.32 seconds, followed by complete non-fast
`./RunQuality.sh --base 84e3a135b18bdddb8134713df0b6896be0cc1a37 --head HEAD`
(the documented local pinned-dependency environment), exit zero. Its coverage
suite passed 241/241 in 96.43 seconds: 2,066/2,105 changed lines covered (98%
display), 235/250 changed branches (94.0%). Format, standards, clang-tidy,
cppcheck, IWYU and scan-build completed. Three independent checkpoint review
axes found no remaining P0–P2 after the distinct-fill/integrated-row test gap
was resolved. This does not clear the older whole-branch findings. Qt is not
built by the sanitizer presets; final whole-branch sanitizer evidence remains
separately pending.

**Isolated native check:** Latest app bundle used test Result
`90e0f8745fd5195a67fb1c8647892880` and the previously named isolated store.
Daily at cursor 200 showed one Buy triangle per reached candle with preserved
wick clearance; Sell fans remained unchanged. Real Max playback visibly
advanced through cursor 873 before reaching 1521/1521 (1,522 Daily candles).
Normal candle width survived the endpoint; Zoom in enabled Follow latest and
Follow latest returned to the newest reached candle without changing the
cursor. Hourly switch and terminal seek reached 23,777/23,777; actual wheel zoom
retained candles/markers, and Follow latest returned to the tail. Scrolling
outside the chart exposed the portfolio and per-fill trade log. Terminal
cash/equity/P&L stayed $94,655.96 / $96,556.06 / ($3,443.94).
A read-only query of the saved test result counted two Buys on 2024-05-16,
while its Daily candle showed one Buy glyph, and counted all 5,261 fill records;
`PRAGMA integrity_check` returned `ok`. This independent database audit is
desktop evidence, not a substitute for the public-seam integration test above.
An attempted Command-End trade-table navigation did not visibly navigate and
is not accepted keyboard coverage. No production store or immutable market
data was modified, and no screenshot/binary was added to the repository.

**Remaining authority:** R3 execution-time persistence design confirmation has
been requested; no answer is recorded. The maintainer approved the bounded R4
full-integer half-even repair design on 2026-10-12: preserve prices/fee policy and
public APIs, RED 1/3/5-share literal cases, then verify canonical portfolios and
overflow boundaries. R4 is authorized but not implemented at this ledger
checkpoint. These remain unimplemented, as do the remaining R-items and
CSV/UTC history conditions. No PR/push/merge or root-checkout update is claimed.

### R4 — Full monetary half-even parity repair

**Status:** Scoped repair accepted at `b5be0c8` on 2026-10-12.
This restores the existing numeric policy without changing public interfaces,
price normalization, buy/sell slippage-price direction, fee policy, persisted
schemas, or dependency versions. Existing saved results are not reinterpreted.

- [x] RED through public `runBacktest`: eleven independently worked literal
  cases cover 1/2/3/5 shares, odd/even full-money ties and adjacent nanodollar
  thresholds. The old implementation incorrectly produced 1,000,001 /
  3,000,005 / 5,000,007 instead of 1,000,002 / 3,000,004 / 5,000,008 at
  $1.0000015. Market value, equity, P&L and final canonical portfolio assertions
  failed; cash stayed unchanged.
- [x] Replace component-only parity with the differing parity of the base and
  remainder-money integers. This tests the sum's parity without computing an
  unchecked sum. The existing checked final addition remains authoritative.
- [x] GREEN the focused literal regression. Add passing boundary coverage,
  not an invented RED: 1,000,000,000 shares marked at $9,223.372036854 remains
  representable (9,223,372,036,854,000,000 microdollars), while one nanodollar
  higher rejects the full-sum overflow with `invalidArgument` and no wrapped
  financial result. Existing invalid quantity/price, affordability and
  accounting-error cases remain registered.
- [x] Full `bte_engine_tests` working-tree run passed 52/52 in 0.220 seconds,
  including canonical records, Batch/Paced/repeat and recorded-result tests.
- [x] Working-tree `./RunTest.sh` passed 243/243 in 91.70 seconds. Complete
  `dev-sanitize` configure/build/CTest passed 200/200 in 51.76 seconds with
  ASan/UBSan instrumentation; these presets exclude Qt. No explicit
  LeakSanitizer run is claimed on this macOS host. The sanitizer builds emitted
  an existing duplicate-SQLite-library linker warning, not a sanitizer finding.
  TSan configure/build and complete tests passed 200/200 in 80.21 seconds.
- [x] Review the numerical semantics/test intent and owning spec; run complete
  exact-HEAD `RunTest.sh`, non-fast `RunQuality.sh` and applicable sanitizer
  presets before accepting the checkpoint. No later code slice starts red.

This semantic correction changes newly computed values/hashes only for inputs
that hit the old erroneous odd-base half-even case. The public-seam literal
canonical-record assertions are the explained regression expectations; the
existing golden-fixture gap R6 is still open and is not claimed solved. A new
important-decision entry is unnecessary: the maintainer-approved change restores
the already normative half-even policy and adds no interface/dependency/format
decision. The runtime remains constant-time and allocation-free at this seam;
no measured speed improvement is claimed. Threading/ownership are unchanged.

**Exact acceptance evidence:** Commit
`b5be0c86100cc7122a3905eaf75f4ea0e1428cb2` passed separate `./RunTest.sh`,
243/243 in 84.54 seconds, then complete non-fast
`./RunQuality.sh --base 84e3a135b18bdddb8134713df0b6896be0cc1a37 --head HEAD`
using the previously documented pinned-dependency environment, exit zero.
The coverage suite passed 243/243 in 95.51 seconds; changed lines were
2,067/2,106 (98% display), changed branches 235/250 (94.0%). Format/standards,
clang-tidy, cppcheck, IWYU and scan-build passed. Independent code/docs/test
axes found no remaining scoped blocker. Precommit sanitizer runs above used
the same production/test code; they are not claimed as postcommit executions.
This ledger update is docs-only and does not claim exact-head gates on a later
documentation commit. No PR, CI submission or merge has occurred.

**Next design stage:** R1–R3 depend on a safe live-writer ownership and
pin-before-visibility protocol before execution-time durability can be accepted.
Proposed direction: use the existing SQLite dependency for a cross-process
writer lease, retain source segments before visibility, atomically persist each
completed bar, stop on persistence failure, and recover only the committed
prefix into an Interrupted draft; explicit Save remains required. Platform
file locks need separate implementations, and PID-based liveness is not reliable
ownership proof. The maintainer approved this combined architectural direction
on 2026-10-13, including retaining and warning about legacy staging without a
lease rather than automatically clearing it. The accepted constraints are now
recorded in the owning specification and decision. Detailed cross-module design
and implementation-plan review remain required before changing module interfaces;
no lease or per-bar durability implementation is claimed.

### Task 29 — Merge verified work, then clean up the named branches

**Status (2026-10-13):** Draft PR #76 is published; CI is red and merge/deletion
have not started. Final branch-content reconciliation and cleanup scope remain
open. The checked inventory below records the historical 2026-10-12 refs, not
the current consolidation checkout or updated main.
The maintainer requests completing all tasks, creating and merging the new PR,
then deleting other user-created branches. This does not authorize premature
branch deletion, erasing uncommitted work, deleting main, or removing files and
worktrees as an implied cleanup operation.

- [x] Inventory this repository's local refs and attached checkouts. Non-main
  local branches are `codex/issue10-replay-ux`, `codex/finish-issue10-plan`,
  `codex/implement-ci-slices`, `feature/issue-10-result-replay`, and
  `backup/issue10-pre-seven-commit-rewrite`. Root is attached to
  `codex/implement-ci-slices`; the Issue 10 worktree is attached to
  `codex/issue10-replay-ux`. No branch was deleted.
- [x] Read actual remote heads with `git ls-remote --heads origin`: main at
  `84e3a135`, `codex/implement-ci-slices` at `a66e888`, and
  `feature/issue-10-result-replay` at `750f452`. The local remote-tracking
  `origin/codex/finish-issue10-plan` is stale, not an existing remote branch.
- [ ] Confirm whether all five named local branches and their still-existing
  remote counterparts, including the backup branch, are the intended targets.
- [ ] Complete plan acceptance, exact submission gates and whole-branch review;
  update the existing attached Draft PR #76, verify all required CI jobs for
  its exact SHA, then
  squash-merge to main. Do not use CI as the first quality check.
- [ ] Verify the merged main contains every scoped code/test/document change.
  Preserve the older root drafts and any unique unmerged content through a
  recoverable, explicitly scoped operation before changing checkouts. Never
  reset/overwrite the dirty root or stage the entire `.worktrees` container.
- [ ] Detach or safely switch clean attached checkouts before deleting their
  confirmed branch refs. Verify local and actual remote targets again against
  their expected identities; preserve main and do not touch unrelated repos.
- [ ] Report the merged PR/main revision, deleted local/remote branch names,
  recovery information and any remaining unpublished code or protected files.
