# Issue 10 Incremental Delivery Inventory

## Scope and evidence

This inventory tracks extraction and publication, not whole-plan acceptance.
The preserved source branch is `codex/issue10-replay-ux` at
`6ac45eaeecfdf1f840a994b951957340bf78fafd`. Relative to fixed base
`84e3a135b18bdddb8134713df0b6896be0cc1a37`, it contains 82 commits and
64 changed paths (11,627 additions and 475 deletions). Those are the original
source totals, not the size of any individual PR.

The maintainer requested small, independently reviewable PRs instead of a
single accumulated change. Preserve the source branch until the final
file/hunk reconciliation proves every intended change is merged or explicitly
retained. Do not force-push or delete its history as part of extraction.

## Delivery groups

A group may need several PRs. Shared files must be split by concern rather
than copied wholesale. A historical checkpoint proves only its recorded scope;
each extracted submission requires fresh applicable checks on its exact SHA.

| ID | Concern and source checkpoints | Publication status / remaining gate |
| --- | --- | --- |
| R | Full-money half-even rounding, `b5be0c8` | [PR #71](https://github.com/sek788432/Stock-Back-Test-System/pull/71) merged as `a0c69c5c6bf2aa8565f1c82887a56e5351f42465`; source `Backtest.cpp` also contains an unmerged S-group lifecycle change |
| T | Trusted Hourly/Daily Replay boundaries, `a098577` | Extracted locally; not submitted. New review found same-day interior-gap fills can appear in Daily without a selected candle. Repair direction is awaiting confirmation; no scoped acceptance claimed |
| F | Real selectable five-Buy/five-Sell offline fixture, `6b9b3e7` | Local source only; extract with its fixture, registration and owning documentation, then prove independence/dependencies and rerun gates |
| M | Directional glyphs `f94fc7a` through `e43c7fb`, candle anchoring `2d5c034` through its `6f6fc86` audit, wick clearance `8879169`/`9ae0f5e`, Buy-only grouping `d209d21` | Local source only; separate rendering, anchoring and grouping concerns. Include ReplayTab's containing-candle adapter wiring and tests. Raw fills and financial records must remain ungrouped; Sell grouping is not approved |
| V | Progressive playback `05c23eb`/`711626d`, wheel zoom `cf4d85b`, axes/volume `6994ae7`, readable sliding viewport `ca3323c`/`e37057a`, stable geometry `01620b9` | Local source only; extract in dependency order. Full three-size/playing-resize, exact-data comparison and long-seek responsiveness acceptance remains open |
| L | CSV-derived development snapshot `d21cfc1`/`c28ddec`, launcher/build provenance `3245574` | Local source only; separate snapshot preparation from provenance, include script/data tests. Final delivered-checkout Launch.sh scenario is pending; this is not a public data release |
| S | Explicit recoverable Save `a5a43cf`, asynchronous Results library `95a03c9` through `12958ea`, naming `b95b344` through `12fe8eb` | Local implementation, not fully accepted. Split Save, Results and naming PRs; do not merge with known live-writer/retention/mid-run durability gaps or incomplete UI acceptance |
| U | Metric units/period explanations `236d97b`/`cf65e2a`, threshold coverage `d8b3c76` | Local source only; extract UI-only changes without S-group code. Label traversal is evidenced; unchanged Engine-record/hash acceptance still needs confirmation |
| P | SQLite manifest-pin regression, `9bb2a9e` | Submitted as [PR #73](https://github.com/sek788432/Stock-Back-Test-System/pull/73). Consult that PR's exact head/base, verification and merge state before treating it as merged; aligning with main must not inherit old-head gates. The assertion matches the actual manifest, not the stale root draft |
| H | Approved CSV/UTC historical-condition semantics, `89d7704` | Requirements only. Previous-data-day, N-day gain, prior-21-day extrema/breakouts and immutable pre-range warm-up are not implemented |
| D | Plan ledger and workspace reconciliation, through `6ac45ea` | Focused inventory [PR #72](https://github.com/sek788432/Stock-Back-Test-System/pull/72) merged as `d1722bd448d097d40153bde1fb4a8b5cd1ce6fcf`. Preserve the original dated ledger and open findings; never label remaining tasks complete merely because a code checkpoint passed |

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

- The current T-group extraction modifies four tracked files only. It was
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
