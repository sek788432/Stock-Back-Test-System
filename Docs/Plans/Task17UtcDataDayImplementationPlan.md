# Task 17 UTC Data-Day Predicate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Add a selectable inclusive Up/Down percentage condition against the previous available UTC data-day's final immutable CSV close, without changing legacy Strategy behavior or adding warm-up data to executed Results.

**Architecture:** The Snapshot reader supplies one bounded pre-range close from the same immutable snapshot. Strategy receives it as an immutable value, evaluates actual bars only, and rolls its close reference at UTC data-day boundaries. Backtest Bindings forwards the reference, Qt exposes positive magnitude plus direction, and canonical Strategy hashing uses v2 framing only when the new source is present.

**Tech Stack:** C++20, Qt 6, CMake, GoogleTest, SQLite-backed Result Store, `ReleaseSnapshotReader`.

**Spec:** [`Task17UtcDataDayPredicateDesign.md`](Task17UtcDataDayPredicateDesign.md), [`StrategyAuthoring.md`](../Specs/StrategyAuthoring.md), [`EngineReplayPnL.md`](../Specs/EngineReplayPnL.md), [`Issue10DeterministicReplayFixturePlan.md`](../Issue10DeterministicReplayFixturePlan.md).

## Global Constraints

- A UTC data day is a UTC date with actual source rows; empty dates do not count and no row is synthesized.
- The daily close is the final chronological source close, including extended-hours rows; it is not an official exchange close.
- Compare the current actual close against the previous available UTC data-day close; Down and Up take positive magnitudes and use inclusive thresholds.
- Read history only from the same immutable snapshot. Missing history is absent/nonmatching; mutable CSV and download fallback are forbidden.
- Warm-up supplies only price-reference state. It adds no bars, indicator updates, signals, orders, fills, portfolio records, or Replay candles.
- Preserve existing `closeChangePercent` behavior and every legacy Strategy v1 hash byte-for-byte. Plans using the new source use v2 framing.
- Preserve next-actual-open execution timing and Replay's read-only behavior.

## Review Focus

- Rows earlier on the current UTC date must not become the prior-day reference; prove with a same-date pre-range row in `ReleaseSnapshotTest.lastCloseBeforeUtcDateExcludesRowsFromBoundaryDate`.
- A later extended-hours row must win over the regular close, even across a weekend or long calendar gap; prove with `ReleaseSnapshotTest.lastCloseBeforeUtcDateReturnsLastAvailableRowAcrossLongGap` and the literal Strategy fixture.
- Missing, zero, invalid, or future-dated history must never match or create a look-ahead; prove with `SelectableStrategyTest.dataDayCloseConditionIgnoresAbsentInvalidAndFutureHistory`.
- Changing source CSV after snapshot creation must not alter the historical reference; prove with `ReleaseSnapshotTest.preRangeDataDayCloseIsStableAfterSourceMutation`.
- History must not leak into engine or Replay timelines; prove with the recorded Engine and `ReplayBacktestDeterminism` integration tests.

---

### Task 1: Read One Prior Data-Day Close from the Immutable Snapshot

**Files:**
- Modify: `Src/Backend/Data/Include/Bte/Data/ReleaseSnapshot.h`
- Modify: `Src/Backend/Data/Private/ReleaseSnapshot.cpp`
- Test: `Tests/Unit/Data/UnitTest_ReleaseSnapshot.cpp`

**Interfaces:**
- Produces `data::SnapshotHistoryClose { core::Timestamp timestamp; std::int64_t closeNanodollars; }`.
- Produces `SnapshotSelection::preRangeDataDayClose` as an optional `SnapshotHistoryClose`.
- Produces `ReleaseSnapshotReader::lastCloseBeforeUtcDate(std::string_view symbol, core::Timestamp utcDateStart, const core::CancellationToken &cancellation = {}) const -> core::Result<std::optional<SnapshotHistoryClose>>`.
- The method returns the last row strictly before the UTC date boundary for the requested symbol. Its segment lookup returns at most one close, regardless of the calendar gap.

- [x] **Step 1: Write failing Snapshot contract tests.** Add `lastCloseBeforeUtcDateExcludesRowsFromBoundaryDate`, `lastCloseBeforeUtcDateReturnsLastAvailableRowAcrossLongGap`, `lastCloseBeforeUtcDateReturnsEmptyWithoutPriorRows`, `lastCloseBeforeUtcDateHonorsCancellation`, `lastCloseBeforeUtcDateRejectsNonMidnightBoundary`, and `preRangeDataDayCloseIsStableAfterSourceMutation`. Use a prior UTC date with close 100 followed by an extended-hours close 110; place a row exactly at the boundary and verify it is excluded; assert the returned close is 110 and no warm-up bar appears in `selection.bars`.
- [x] **Step 2: Verify RED.** `cmake --build Output/Issue10Baseline --target bte_data_tests --parallel 4` failed only on the expected missing reader API/selection field; no test syntax or unrelated build error occurred.
- [x] **Step 3: Implement the bounded reader API and selection field.** Search each matching symbol segment for its last timestamp strictly below the UTC-midnight boundary, choose the greatest timestamp, and return its timestamp and fixed-point close. Reject non-midnight boundaries, return an empty optional when no earlier row exists, honor cancellation, and do not read source CSV after the snapshot is built.
- [x] **Step 4: Verify GREEN.** `ctest --test-dir Output/Issue10Baseline --output-on-failure -R '^(CsvBarStreamTest|ReleaseSnapshotTest|SegmentRetentionTest)\.'` passed all 44 Data tests, including all six new contracts.
- [x] **Step 5: Review the Snapshot seam.** `git diff --check`, clang-format 18.1.8, changed-line clang-tidy, and the Data tests passed; the diff was reviewed against the immutable-reader contract. Commit this green Task 1 checkpoint. The full branch review and submission gates remain in Task 6.

### Task 2: Add Strategy Evaluation and Versioned Identity

**Files:**
- Modify: `Src/Backend/Strategy/Include/Bte/Strategy/SelectableStrategy.h`
- Modify: `Src/Backend/Strategy/Private/SelectableStrategy.cpp`
- Test: `Tests/Unit/Strategy/UnitTest_SelectableStrategy.cpp`

**Interfaces:**
- Produces `strategy::PreviousDataDayClose { core::Timestamp timestamp; double close; }`.
- Add `ConditionSource::previousDataDayCloseChangePercent` after existing enum values.
- Extend `SelectableStrategy::create(SelectableStrategyPlan plan, std::optional<PreviousDataDayClose> previousDataDayClose = std::nullopt)`; existing one-argument calls remain source-compatible.
- Down is encoded as `Comparison::lessThanOrEqual` with threshold `-X`; Up is `greaterThanOrEqual` with threshold `+X`. Require the percent domain and `abs(threshold) > 0`.

- [x] **Step 1: Write failing Strategy tests.** Added `dataDayCloseConditionUsesInclusivePositiveMagnitude`, `dataDayCloseConditionKeepsReferenceAcrossIntradayBars`, `dataDayCloseConditionRollsAtUtcDateBoundaryAfterFridayExtendedClose`, `dataDayCloseConditionUsesUtcDatesAcrossYearLeapAndExchangeDst`, `dataDayCloseConditionIgnoresAbsentInvalidAndFutureHistory`, `dataDayCloseConditionRejectsInvalidDirectionAndDomain`, `legacySelectableHashRemainsV1`, and `dataDayClosePlanUsesV2Hash`. Coverage includes the 95/100 Down and 105/100 Up inclusive edges with adjacent values, 104.5/110, stable intraday references, the Friday 22:00 UTC final close used at Monday 00:00 UTC, year/leap/DST dates, absent and invalid seeds, and v1/v2 identity fields. The legacy fixture remains `d8722c8bb0625c68107e91460de923d06ef0847c10ae4e3294ec76c028b6aa85`.
- [x] **Step 2: Verify RED.** `cmake --build Output/Issue10Baseline --target bte_strategy_tests --parallel 4` failed only because the new `ConditionSource`, `PreviousDataDayClose`, and factory overload were not yet defined.
- [x] **Step 3: Implement the Strategy source and value context.** Track the validated seed and final observed close for the current UTC date. Evaluate each Task 17 predicate against the prior data-day close before recording the current bar. UTC date flooring runs only for plans that contain this source. Invalid, zero, same-date, and future seeds are treated as absent. The source accepts only signed inclusive percentage comparisons with nonzero magnitude.
- [x] **Step 4: Implement hash framing.** The starter and plans without the new source retain v1 framing and the frozen literal digest. Plans containing the new source use v2 framing with the established field encoding. The v2 fixture is `87d4d78867b5cc107f74913b4c917f695a70fc43562b9715e1eb13c94dbeabbb`; tests verify source, direction, threshold, condition order, and group logic affect the hash.
- [x] **Step 5: Verify GREEN.** The focused Strategy suite passed 23/23; the complete workspace build passed; complete CTest passed 290/290; and `python3 Tools/CheckProjectStandards.py --base 508d2d9e18befe2ebb1e80b9a6d8bf528c5c5def` passed. clang-format, `git diff --check`, production clang-tidy, and changed-line test clang-tidy passed. The final exact-head `RunTest.sh` and `RunQuality.sh` gates remain in Task 6.

### Task 3: Carry Snapshot History Through Backtest and Recorded Replay

**Files:**
- Modify: `Src/Backend/Engine/Include/Bte/Engine/Backtest.h`
- Modify: `Src/Backend/Engine/Private/Backtest.cpp`
- Modify: `Src/Bindings/Backtest/BacktestSessionVm.cpp`
- Test: `Tests/Unit/Engine/UnitTest_Backtest.cpp`
- Test: `Tests/Unit/Engine/UnitTest_RecordedBacktest.cpp`
- Test: `Tests/Unit/Replay/UnitTest_ReplayBacktestDeterminism.cpp`

**Interfaces:**
- Add `BacktestRequest::previousDataDayClose` as `std::optional<strategy::PreviousDataDayClose>`.
- `BacktestExecution::start()` forwards that value only to `SelectableStrategy::create()`; it does not enqueue or process it as a bar.
- The persisted binding attaches `SnapshotHistoryClose` from the same reader and snapshot ID to `SnapshotSelection::preRangeDataDayClose`, converts the fixed-point close to dollars, and forwards it with the actual bars.

- [x] **Step 1: Write failing Engine and recorded Replay tests.** Added `selectableStrategyHistoryDoesNotAddExecutedBarsOrRecords` and `persistedDataDayPredicateReplaysOnlySelectedBars`. They assert actual selected-bar counts, first-signal order timing, next actual open fill, and no pre-range Replay candle or record.
- [x] **Step 2: Verify RED.** The Engine and Replay test targets failed to compile only because `BacktestRequest::previousDataDayClose` was missing.
- [x] **Step 3: Implement request forwarding and persisted history selection.** The persisted binding floors the first selected bar timestamp to UTC midnight, calls `lastCloseBeforeUtcDate()` on the same open reader, populates the separate pre-range selection value, and passes the converted Strategy context. `.bars`, Result spans, record timestamps, and Replay selections remain limited to the requested execution range.
- [x] **Step 4: Verify GREEN.** The focused Engine, Recorded Backtest, and Replay CTest selection passed 64/64, including both new contracts and existing record/hash checks. The complete exact-head gates remain in Task 6.

### Task 4: Expose Positive Down/Up Controls and Honest UI Help

**Files:**
- Modify: `Src/Frontend/Backtest/BacktestTab.cpp`
- Test: `Tests/Unit/Frontend/UnitTest_BacktestTab.cpp`

**Interfaces:**
- Add `ConditionMetric::previousDataDayCloseChangePercent` and map it to the Strategy source.
- For this metric, the existing comparison combo exposes only **Down at least** and **Up at least**, mapped to signed inclusive comparisons. The threshold control accepts positive percent magnitude; all other metrics retain their existing comparison choices and threshold ranges.

- [x] **Step 1: Write failing Qt tests.** Extended `selectableMetricsSubmitTypedPlansToBackend` and added `previousDataDayCloseMetricUsesPositiveDownUpAndExplainsUtcSource`. The test asserts Down `lessThanOrEqual/-5.0`, Up `greaterThanOrEqual/+2.5`, positive-only input, and tooltip/accessibility wording for CSV/UTC data days, extended-hours source close, current-date exclusion, inclusive thresholds, and absent history.
- [x] **Step 2: Verify RED.** The UI target compiled; the new Qt test failed because the metric still exposed six generic comparisons instead of Down/Up.
- [x] **Step 3: Implement the metric, direction mapping, and help text.** The editor updates comparisons per metric, restores all legacy options and signed threshold ranges for other metrics, and explains that Task 17 uses UTC data days rather than bars. The period field remains unused by the formula.
- [x] **Step 4: Verify GREEN.** The complete `bte_backtest_frontend_tests` CTest target passed, including the new Task 17 contract, every existing metric mapping, and metric units/accessibility descriptions. Changed-line clang-tidy passed.

### Task 5: Update Owning Specifications and Issue 10 Status

**Files:**
- Modify: `Docs/Specs/StrategyAuthoring.md`
- Modify: `Docs/Specs/DataLayer.md`
- Modify: `Docs/Specs/EngineReplayPnL.md`
- Modify: `Docs/Specs/BacktestReplayProduct.md`
- Modify: `Docs/Issue10DeterministicReplayFixturePlan.md`
- Modify: `Docs/Plans/Issue10RemainingWork.md`
- Modify: `Docs/Plans/Task17UtcDataDayPredicateDesign.md`
- Modify: `Docs/Decisions/ImportantDecisions.md`

**Interfaces:**
- Document only the implemented Task 17 behavior after Tasks 1–4 pass. Keep Task 18 labeled accepted/not implemented and do not claim official-session close behavior.

- [x] **Step 1: Update the owning specs and fixture evidence.** After Tasks 1–4 focused tests passed, marked Task 17 implemented locally with exact-head gates pending. Documented immutable snapshot lookup, the one-row bounded pre-range context, UTC state rollover, positive Down/Up UI comparisons, v1/v2 hashing, missing-history behavior, and unchanged Result/Replay timelines. The focused contracts passed: Data 44/44, Strategy 23/23, Engine/Recorded/Replay 64/64, and the complete `bte_backtest_frontend_tests` target.
- [x] **Step 2: Update decision traceability and Issue 10 status.** Extended the historical-condition decision with the approved pre-range/hash choices, removed Task 17 from unfinished feature behavior, and retained exact-head quality, sanitizer, remote, and native acceptance as pending. Task 18 and the other open Issue 10 items remain listed.
- [x] **Step 3: Verify documentation.** `git diff --check` and `Tools/CheckProjectStandards.py --base 508d2d9e18befe2ebb1e80b9a6d8bf528c5c5def` passed. Manually verified the added relative links and the StrategyAuthoring anchor; current specifications label Task 18 accepted/not implemented.

### Task 6: Verify the Complete Exact-Head Change

**Files:**
- Review all Task 17 code, tests, docs, build wiring, and commits.

- [x] **Step 1: Run the complete test suite before commit.** Final code commit `bed5388` passed `./RunTest.sh`: 295/295 in 93.71 seconds. Earlier Qt/CTest checkpoints are retained in the commit history; this exact-head run supersedes them.
- [x] **Step 2: Review the complete change before commit.** Reviewed `508d2d9..bed5388`, including implementation, tests, docs, build wiring, and the `ee86535` direct `<compare>` include fix for IWYU. Fixed the UI zero-threshold gap, missing-field warnings, IWYU finding, and remaining changed-line coverage gaps. No unresolved P0–P2 findings remain. Parallel reviewers were unavailable due to account usage limits, so code, documentation, and test axes were reviewed sequentially.
- [x] **Step 3: Commit the reviewed implementation and documentation.** The source and test slices are committed as `5fe4505`, `2bcf948`, `ef66953`, `f5b939d`, `f5c61e8`, `22282d2`, `6dbf1b3`, `ee86535`, and `bed5388`; documentation checkpoints are `7b004dc`, `e24868c`, `1b442b9` (local gates), and `610d846` (remote PR access status).
- [x] **Step 4: Run exact-head project quality.** On `bed5388`, `./RunTest.sh` passed 295/295 in 93.71 seconds. `./RunQuality.sh --base 508d2d9e18befe2ebb1e80b9a6d8bf528c5c5def --head HEAD` exited 0: standards passed (10,336,874 lines / 759 files), full Clang 18 quality build, clang-tidy, cppcheck, IWYU, and scan-build (`No bugs found`) passed; coverage tests passed 295/295, changed-line coverage was 100% (193/193), and changed-branch coverage was 97.2% (35/36).
- [x] **Step 5: Run sanitizers on the exact code commit.** The `dev-sanitize` ASan/UBSan configuration passed CTest 249/249 in 58.42 seconds, and `dev-tsan` passed 249/249 in 93.77 seconds, with no sanitizer reports. These runs used the exact source/test tree at `bed5388`.
- [ ] **Step 6: Record remote acceptance.** The verified branch is published, but the connected GitHub integration rejected PR creation with HTTP 403 (`Resource not accessible by integration`); the browser session is signed out and no `gh` CLI is installed. A PR must be opened after repository integration access includes pull-request write permission (or by an authenticated maintainer); then require project CI and CodeQL to pass before merging.
- [ ] **Step 7: Merge and continue cleanly.** After required CI and CodeQL pass, merge the PR under the user's standing authorization, fast-forward local `main` to `origin/main`, audit and remove the completed Task 17 branch, then create a fresh branch from `main` for the other unfinished Issue 10 items.

## Completion Criteria

- All new and existing focused tests pass; exact-head `RunTest.sh`, full `RunQuality.sh`, and both sanitizer configurations pass on `bed5388`.
- Legacy Strategy v1 hashes and prior-actual-bar behavior remain unchanged; Task 17 plans use v2 framing.
- Recorded Results include only actual execution bars; Replay remains read-only and presents no warm-up data.
- Documentation marks only Task 17 as implemented and lists remaining Issue 10 work accurately.

## Plan Self-Review

- Spec coverage is complete: Task 1 proves immutable bounded lookup; Task 2 proves predicate, date rollover, validation, and hash compatibility; Task 3 proves Engine, Result, and Replay boundaries; Task 4 proves authoring behavior and accessible disclosure; Task 5 updates the owning documentation; Task 6 verifies the reviewed exact commit.
- Task 18 historical extrema, gain, and breakouts remain a separate implementation slice as required by the design.
- After Task 17 merges, continue the remaining Issue 10 work on a new branch based on the updated `main`.
- Native visual Replay and delivered `Launch.sh` acceptance remain separately tracked in `Issue10RemainingWork.md`; this plan does not claim those broader Issue 10 checks are complete.
