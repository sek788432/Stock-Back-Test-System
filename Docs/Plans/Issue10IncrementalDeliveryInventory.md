# Issue 10 Incremental Delivery Inventory

## Scope and evidence

This inventory tracks extraction and publication, not whole-plan acceptance.
The preserved source branch is `codex/issue10-replay-ux` at
`6ac45eaeecfdf1f840a994b951957340bf78fafd`. Relative to fixed base
`84e3a135b18bdddb8134713df0b6896be0cc1a37`, it contains 82 commits and
64 changed paths (11,627 additions and 475 deletions). Those are the original
source totals, not the size of any individual PR.

The earlier maintainer request was small, independently reviewable PRs.
On 2026-10-13 the maintainer instead requested one consolidated commit/PR
for the current implementation, followed by completion of the remaining plan.
Preserve the source branch until the final
file/hunk reconciliation proves every intended change is merged or explicitly
retained. Do not force-push or delete its history as part of extraction.

## Delivery groups

### Current consolidation checkpoint

- Published #76 rotation repair `d50dabb` passed full exact local gates but
  Linux coverage still failed its five-second load wait in `38583994672`.
  A separate macOS checkout `early EOF` was requested to rerun; it was not a
  test failure or a passing job. Independently safe rotation-only PR #78 at
  `e4f9e0c` contains exactly two Core implementation/test files, with full
  exact local tests/Quality and both backend sanitizers passed. Every required
  remote check passed; it was squash-merged into `main` as `4750042` on
  2026-10-15. The bounded-array / in-place SHA follow-up is committed at
  `083cf26`. Its exact-head RunTest passed 246/246, complete Quality passed
  against `4750042` (98% changed lines; 236/252 branches), and ASan/UBSan and
  TSan each passed 203/203. Independent digest/subspan tests and three faster
  load runs passed; remote confirmation and independent follow-up review remain
  required. No unfinished persistence/chart/CSV behavior was smuggled
  into #78, and no known #76 blocker is declared closed. Detailed raw samples,
  stale-counter diagnostic failure and limitations are in
  [remaining work](Issue10RemainingWork.md).
- Latest published #76 head `e45ac7b` passed full exact local tests/Quality
  against main `2a21657`. Remote run `38538578246` passed standards, static
  analysis, platform tests and both sanitizer jobs; CodeQL passed. Coverage
  still failed the long-range load wait at 5,002 ms. A measured local SHA-256
  fixed-rotation repair preserves public digest behavior and all validation:
  three coverage-build load samples improved from 2,376/2,357/2,280 ms to
  1,727/1,698/1,760 ms. Complete repair-head gates, publication and remote
  acceptance remain pending; this does not resolve persistence or native
  acceptance blockers. See [remaining work](Issue10RemainingWork.md) for the
  reproducible command and limitations.
- Branch: `codex/issue10-consolidated`, fixed main base
  `999c1b90a2748fe09fbe6d1003f64633f124c285`; original source tip remains
  `6ac45eaeecfdf1f840a994b951957340bf78fafd` without rewriting its 82 commits.
- A normal squash integration gathers the remaining tracked source paths.
  Already merged rounding and fixture tests are retained, including the stronger
  literal metric descriptions, `[[nodiscard]]` helpers and Engine public-link
  dependency closure. No root untracked drafts, personal documents, snapshots,
  build output or existing stashes are included or deleted.
- The source period-disable behavior and its weaker label-only expectations
  are not reapplied: #74's editable identity metadata and literal tests remain.
  No period normalization or compatibility decision is implied.
- The integration retains the source Save/Results/naming, chart/viewport,
  snapshot-launch/provenance and Replay changes as a checkpoint, not acceptance.
  Known live-writer, pin-visibility, durability, Daily interior-gap and
  seven-column source-timeframe findings remain blockers to merging main.
  The original source's stricter CSV trailing-column rejection is also retained
  as an unaccepted checkpoint, not an approved compatibility change; the separate
  narrow CSV stash preserves the earlier compatibility-aware repair proposal.
- Historical-condition semantics remain requirements only. The independent
  canonical-hash oracle, exact saved-data comparison, long seek/resizing and
  final delivered-checkout acceptance remain unfinished.
- The initial consolidated checkpoint was published as Draft
  [PR #76](https://github.com/sek788432/Stock-Back-Test-System/pull/76), head
  `d7ce3d1bd546cb8f5610ac54ab67c5a5951b0c75`, not merged or accepted. This is
  historical publication evidence; the later published head is recorded below.
  Exact-head RunTest passed 243/243 (87.94 seconds); complete non-fast Quality
  exited zero with 243/243 coverage tests (95.59 seconds), reported 98% changed
  lines and 93.8% changed branches. Separate backend-only ASan and TSan presets
  passed 200/200 (51.35 and 78.66 seconds); these do not cover all Qt workers.
  Remote CI then failed macOS/Linux test compilation, Linux IWYU and the
  long-range Replay coverage test. Repairs and fresh exact-head gates are
  pending; local success is not evidence of remote success. Independent
  review found no new conflict-resolution defects; four documentation findings
  were corrected. Known original semantic blockers still mean Needs changes.
- The initial uncommitted integration RunTest passed 243/243 (91.35 seconds).
  This is working-tree characterization, not exact-submission evidence. Owning
  `Overview.md` is additionally updated to align the documented explicit Save
  gate; it is not one of the original 64 source paths.
- PR #73 was closed as superseded after #76 publication and the identical pin
  test blob were verified; no assertion or original branch was discarded.
- The maintainer approved the SQLite lease / pin-before-visibility / atomic
  completed-bar / committed-prefix recovery direction on 2026-10-13. Legacy
  unleased staging must be preserved with a warning. The owning spec and
  decision now record this direction; detailed design and implementation
  remain pending, not accepted as already repaired.
- Local follow-up `1fb6b10be0401c6a555e59a1dfc6af23dded4152` passed exact-head
  RunTest 243/243 (84.65 seconds), but complete Quality exited 1 at IWYU:
  libc++ considered the new Linux/libstdc++ provider headers unused. This head
  was not pushed. The next repair conditions only those headers on libstdc++;
  it adds no suppression or gate change. Fresh complete exact-head gates and
  remote confirmation remain required. The long-load coverage failure still
  has diagnostics only, not an accepted performance repair.
- Published `b7e27a91ffcbb441b1d8ab0042177a7245530d46` passed exact-head
  RunTest 243/243 (90.15 seconds) and complete Quality, including 243/243
  coverage tests (94.80 seconds), 98% reported changed lines and 93.8% branches.
  Remote CI still failed: remaining Qt waiting macros narrow in GCC, one
  libstdc++ macro-provider include is missing, Results rename testing segfaults
  while the asynchronous refresh clears cells, and long-range Replay loading
  exceeds its unchanged five-second wait. The rename crash was reproduced
  locally (exit 139); a null-safe observed-name predicate then passed the same
  real widget interaction. Further full/exact-head checks remain pending.
- On 2026-10-14 the maintainer requested safe completed-only merges while the
  broad checkpoint is repaired. PR #73 was reopened and aligned to exact
  `9b1c492`; its two-file manifest-test/inventory diff retains prior exact-head
  RunTest 219/219 and full Quality evidence, plus a fresh focused test. Current
  head CI/CodeQL and every required gate then succeeded. Expected-SHA-guarded
  squash merge landed as `34b958d8283a5d142e190a40bce0b9fb868654d7`, and the
  remote main ref was verified at that commit. This does not accept Draft #76.
  The [remaining-work summary](Issue10RemainingWork.md) separates data safety,
  real-data completeness, CI/performance and final native acceptance.
- Local follow-up `32e5034854567e1fa2ac07240c52d815c2f25f97` commits the
  remaining 47 portable Qt waits, the libstdc++ macro-provider include, the
  null-safe rename-test predicate and current tracking. Exact RunTest passed
  243/243 (89.37 seconds). Complete non-fast Quality against main `34b958d`
  failed changed-line coverage: 1,995/2,038 lines (approximately 97.89%, below
  the unchanged 98% gate). Static analysis completed and coverage tests passed
  243/243 (88.96 seconds); the aggregate workflow still exited 1. Initial
  environment/DNS stops were resolved using the documented pinned VCPKG_ROOT
  and approved networking, not a tooling/gate change. At that failed-gate
  checkpoint this repair was not pushed; coverage and remote acceptance were
  still open. The broad checkpoint still has
  known semantic blockers regardless of mechanical gate status.
- A subsequent test-only repair adds real Results catalog failure and refresh
  recovery coverage: an isolated regular file blocks the store directory;
  the tab must show the error with no rows and disabled result actions, then
  recover to an empty valid catalog after the fixture path is repaired. The
  focused case passed and failed against a temporary wrong error display;
  that mutation was removed. Independent scoped review found no new P0–P2.
  Fresh full exact-head gates and remote confirmation remain required.
- The test-only follow-up was published as `98819b334f82c63ec76c370f25f4b0797ccbee82`
  after exact RunTest passed 243/243 (87.99 seconds) and complete non-fast
  Quality against `34b958d` exited zero: clean coverage tests 243/243 (87.70
  seconds), changed lines 1,999/2,038 and branches 224/240. This repairs the
  previous aggregate coverage failure without weakening a gate. Remote
  CodeQL succeeded, but project CI did not start: the PR was confirmed
  merge-conflicted in this inventory, not fully green. Main concurrently
  advanced via data-only #77 to `2a21657`; a normal merge preserves its CSV
  changes and reconciles this document's compatible latest/historical facts.
  Fresh merge-head local gates and required remote CI remain pending.

The table below preserves the preceding incremental publication checkpoints;
it is not a claim that the unaccepted consolidation has merged.

A group may need several PRs. Shared files must be split by concern rather
than copied wholesale. A historical checkpoint proves only its recorded scope;
each extracted submission requires fresh applicable checks on its exact SHA.

| ID | Concern and source checkpoints | Publication status / remaining gate |
| --- | --- | --- |
| R | Full-money half-even rounding, `b5be0c8` | [PR #71](https://github.com/sek788432/Stock-Back-Test-System/pull/71) merged as `a0c69c5c6bf2aa8565f1c82887a56e5351f42465`; source `Backtest.cpp` also contains an unmerged S-group lifecycle change |
| T | Trusted Hourly/Daily Replay boundaries, `a098577` | Extracted locally; not submitted. New review found same-day interior-gap fills can appear in Daily without a selected candle. Repair direction is awaiting confirmation; no scoped acceptance claimed |
| F | Real selectable five-Buy/five-Sell offline fixture, `6b9b3e7` | Test-only [PR #75](https://github.com/sek788432/Stock-Back-Test-System/pull/75) merged as `999c1b90a2748fe09fbe6d1003f64633f124c285`; submitted `ba780948ca5b788c411791d5f5cfd934af2e239b` passed full RunTest 219/219, complete Quality, independent review and all CI/CodeQL. Additional Qt-enabled focused ASan/TSan each passed seven tests. Save lifecycle documentation was excluded from #75; its literal fixture/tests and public-link closure are retained in consolidation. The independent hash oracle and whole-plan acceptance remain open |
| M | Directional glyphs `f94fc7a` through `e43c7fb`, candle anchoring `2d5c034` through its `6f6fc86` audit, wick clearance `8879169`/`9ae0f5e`, Buy-only grouping `d209d21` | Local source only; separate rendering, anchoring and grouping concerns. Include ReplayTab's containing-candle adapter wiring and tests. Raw fills and financial records must remain ungrouped; Sell grouping is not approved |
| V | Progressive playback `05c23eb`/`711626d`, wheel zoom `cf4d85b`, axes/volume `6994ae7`, readable sliding viewport `ca3323c`/`e37057a`, stable geometry `01620b9` | Local source only; extract in dependency order. Full three-size/playing-resize, exact-data comparison and long-seek responsiveness acceptance remains open |
| L | CSV-derived development snapshot `d21cfc1`/`c28ddec`, launcher/build provenance `3245574` | Narrow seven-column extraction is preserved in a named local stash: its old-main regression failed with unsupported columns, then 39 Data tests passed. Review found daily exports can share that layout and be mislabeled hourly; explicit source-timeframe API/CLI confirmation remains pending, so the narrow slice is not accepted or published. Its explicit-form trailing-column compatibility is retained in the stash; the broad checkpoint's stricter source rejection remains unaccepted. Launcher preparation/provenance and final delivered-checkout Launch.sh remain separate; no public data release |
| S | Explicit recoverable Save `a5a43cf`, asynchronous Results library `95a03c9` through `12958ea`, naming `b95b344` through `12fe8eb` | Included in the current consolidated checkpoint per the latest maintainer request, not fully accepted. The earlier split-PR preference is superseded; known live-writer/retention/mid-run durability gaps and incomplete UI acceptance still prohibit merging main |
| U | Metric units/period explanations `236d97b`/`cf65e2a`, threshold coverage `d8b3c76` | Display-only [PR #74](https://github.com/sek788432/Stock-Back-Test-System/pull/74) merged as `8ab5ea4b8db7f01d5591a748805b0ae0eb544dfe`; exact submitted head `569bbe3` passed local full tests/quality, both backend-only sanitizer presets and all CI/CodeQL checks. Period editability and typed identity are unchanged. Excluded source period-disable/normalization work and whole Task 16 Engine-record/hash acceptance remain open |
| P | SQLite manifest-pin regression, `9bb2a9e` | [PR #73](https://github.com/sek788432/Stock-Back-Test-System/pull/73) reopened on 2026-10-14, then merged as `34b958d8283a5d142e190a40bce0b9fb868654d7` after every exact-head CI/CodeQL gate succeeded; two-file diff at `9b1c492`, exact local full tests/Quality and fresh focused test passed. The same assertion remains in Draft #76; no source ref was discarded |
| H | Approved CSV/UTC historical-condition semantics, `89d7704` | Requirements only. Previous-data-day, N-day gain, prior-21-day extrema/breakouts and immutable pre-range warm-up are not implemented |
| D | Plan ledger and workspace reconciliation, through `6ac45ea` | Focused inventory [PR #72](https://github.com/sek788432/Stock-Back-Test-System/pull/72) merged as `d1722bd448d097d40153bde1fb4a8b5cd1ce6fcf`. The original full dated ledger remains local. Preserve evidence and open findings; never label remaining tasks complete merely because a code checkpoint passed |

The source contains corrective/style/test commits between the listed checkpoints.
They remain reachable from the pinned source tip and are part of the final
reconciliation; a checkpoint list is not permission to omit those fixes.

## Complete original changed-path mapping

Every one of the 64 original paths is listed once below. Multiple group IDs
mean multiple concerns share a file; delivery of one group does not publish
the other hunks in that file.

| Original changed path | Group IDs |
| --- | --- |
| `Docs/BUILD.md` | L |
| `Docs/Decisions/ImportantDecisions.md` | F, S, H |
| `Docs/Issue10DeterministicReplayFixturePlan.md` | D |
| `Docs/Plans/ReplayChartViewportImplementationPlan.md` | V |
| `Docs/Research/TradingViewChartInteraction.md` | V |
| `Docs/Specs/BacktestReplayProduct.md` | F, S, V |
| `Docs/Specs/DataLayer.md` | L, S |
| `Docs/Specs/EngineReplayPnL.md` | R, T, F, S, H, V |
| `Docs/Specs/FrontendQt.md` | M, V, S, U, L |
| `Docs/Specs/Indicators.md` | U |
| `Docs/Specs/ReplayChartViewportDesign.md` | M, V |
| `Docs/Specs/StrategyAuthoring.md` | F, U, H |
| `Launch.sh` | L |
| `Src/App/MainWindow.cpp` | L, S |
| `Src/Backend/Data/Private/ReleaseSnapshot.cpp` | L |
| `Src/Backend/Engine/Include/Bte/Engine/Backtest.h` | S |
| `Src/Backend/Engine/Private/Backtest.cpp` | R, S |
| `Src/Backend/Results/Include/Bte/Results/ResultStore.h` | S |
| `Src/Backend/Results/Private/ResultStore.cpp` | S |
| `Src/Backend/Results/Private/ResultStoreTestHooks.h` | S |
| `Src/Bindings/Backtest/BacktestSessionVm.cpp` | S |
| `Src/Bindings/Backtest/BacktestSessionVmTestHooks.h` | S |
| `Src/Bindings/CMakeLists.txt` | S |
| `Src/Bindings/Include/Bte/Bindings/BacktestSessionVm.h` | S |
| `Src/Bindings/Include/Bte/Bindings/ResultLibraryRequests.h` | S |
| `Src/Bindings/Include/Bte/Bindings/ResultReplay.h` | V |
| `Src/Bindings/Replay/ResultLibraryRequests.cpp` | S |
| `Src/Bindings/Replay/ResultLibraryRequestsTestHooks.h` | S |
| `Src/Bindings/Replay/ResultReplay.cpp` | T, V |
| `Src/Frontend/Backtest/BacktestTab.cpp` | S, U |
| `Src/Frontend/CMakeLists.txt` | V, S |
| `Src/Frontend/Charts/QtChartsCandlestickView.cpp` | M, V |
| `Src/Frontend/Charts/ReplayBarViewport.cpp` | V |
| `Src/Frontend/Include/Bte/Frontend/BacktestTab.h` | S |
| `Src/Frontend/Include/Bte/Frontend/IChartView.h` | M, V |
| `Src/Frontend/Include/Bte/Frontend/QtChartsCandlestickView.h` | M, V |
| `Src/Frontend/Include/Bte/Frontend/ReplayBarViewport.h` | V |
| `Src/Frontend/Include/Bte/Frontend/ReplayTab.h` | V |
| `Src/Frontend/Include/Bte/Frontend/ResultsTab.h` | S |
| `Src/Frontend/Replay/ReplayTab.cpp` | M, V, S |
| `Src/Frontend/Replay/ReplayTabSections.cpp` | V |
| `Src/Frontend/Replay/ReplayTabSections.h` | V |
| `Src/Frontend/Replay/ReplayTabStyle.cpp` | V |
| `Src/Frontend/Results/ResultsTab.cpp` | S |
| `Tests/CMakeLists.txt` | F, V, S |
| `Tests/Fixtures/ReplayMultiFill/SYN.csv` | F |
| `Tests/Unit/Bindings/UnitTest_ResultReplay.cpp` | T, V |
| `Tests/Unit/Data/UnitTest_ReleaseSnapshot.cpp` | L |
| `Tests/Unit/Engine/UnitTest_Backtest.cpp` | R |
| `Tests/Unit/Engine/UnitTest_RecordedBacktest.cpp` | S |
| `Tests/Unit/Frontend/UnitTest_BacktestSessionVm.cpp` | S |
| `Tests/Unit/Frontend/UnitTest_BacktestTab.cpp` | S, U |
| `Tests/Unit/Frontend/UnitTest_MainWindow.cpp` | L, S |
| `Tests/Unit/Frontend/UnitTest_QtChartsCandlestickView.cpp` | M, V |
| `Tests/Unit/Frontend/UnitTest_ReplayBarViewport.cpp` | V |
| `Tests/Unit/Frontend/UnitTest_ReplayTab.cpp` | V |
| `Tests/Unit/Frontend/UnitTest_ResultLibraryRequests.cpp` | S |
| `Tests/Unit/Frontend/UnitTest_ResultReplayTab.cpp` | M, V, S |
| `Tests/Unit/Frontend/UnitTest_ResultsTab.cpp` | S |
| `Tests/Unit/Replay/UnitTest_ReplayBacktestDeterminism.cpp` | F, V, S |
| `Tests/Unit/Replay/UnitTest_ReplayFixture.cpp` | T |
| `Tests/Unit/Results/UnitTest_ResultStore.cpp` | S |
| `Tests/Unit/Tools/UnitTest_DependencyManifest.py` | P |
| `Tests/Unit/Tools/UnitTest_ProjectScripts.py` | L |

## Additional workspace state

- The earlier narrow T-group extraction modified four tracked files only. It was
  retained in a named local stash before switching to this inventory branch;
  it has not been discarded, pushed, or claimed accepted.
- The older root `Docs/Issue10DeterministicReplayFixturePlan.md` and
  `Tests/Unit/Tools/UnitTest_DependencyManifest.py` are untracked drafts.
  Preserve them; do not stage them blindly or overwrite them on checkout.
  The latter's unique pin assertion was reconciled into the P-group source
  test at `9bb2a9e`, with current dependency-object/port-version expectations.
- The untracked `.worktrees/` container is not a source artifact to stage.
  Build outputs, development snapshots and isolated test Results are excluded.
- Older feature/planning branches and the backup branch remain preserved.
  Previous merged PRs are not a reason to push their rewritten/duplicated
  histories as new product changes. Recheck unique content before cleanup.
- Local root `main` and the root checkout have not been updated by PR #71.
  The verified merge is on remote main; final checkout reconciliation is pending.
- No recurring/background PR or merge automation was enabled.

## Per-PR publication protocol

1. Pin source commits and a fixed PR base; inventory committed and uncommitted
   changes separately.
2. Extract one coherent concern and include its necessary tests, registration
   and owning documentation. Retain unrelated work recoverably.
3. Reproduce bug regressions on the old extracted behavior. Distinguish added
   passing coverage from actual RED evidence.
4. Complete independent code, documentation and test review. Keep known
   defects and unresolved design decisions explicit.
5. On the exact committed submission SHA, run `./RunTest.sh`, then complete
   non-fast `./RunQuality.sh --base <base> --head HEAD` and applicable
   sanitizer presets before requesting CI. Docs-only changes use their actual
   applicable validation, not invented code gates.
6. Fill the PR template, attach every created PR, and record URL/head/base,
   dependency and acceptance state. A draft is not acceptance or merge approval.
7. Verify all applicable CI checks for that SHA before authorized squash merge.
   Never bypass a failed/missing check.
8. Record the merged SHA and reconcile the group against the source diff.
   Delete branches only after all intended content is accounted for and
   exact cleanup targets are confirmed.

## Final no-omission check

Compare the final merged tree against the pinned source for all listed paths.
Review every remaining hunk; do not rely only on commit titles or patch IDs,
because extraction/squash changes ancestry. Each difference must be recorded
as merged-equivalent, a deliberate reviewed correction, pending publication,
requirements-only, superseded, or preserved user-owned work. Until that
reconciliation and the remaining acceptance work finish, Issue 10 remains
**Needs changes**.
