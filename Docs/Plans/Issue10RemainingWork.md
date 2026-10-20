# Issue 10 Remaining Work

**As of 2026-10-20: Needs changes.** This is a current delivery/acceptance
summary, not a replacement for the full dated
[implementation plan](../Issue10DeterministicReplayFixturePlan.md) or the
[source inventory](Issue10IncrementalDeliveryInventory.md).

GitHub Issue #10 itself is narrowly scoped to the offline deterministic Replay
fixture. Its committed fixture and both named snapshot-determinism tests are
present; this report tracks the broader Issue 10 delivery plan. The issue is
left open for the maintainer to close.

PR #76 is now merged, including R1–R3 persistence safety and the previously
published Replay repairs. Task 17/18, seven-column CSV timeframe provenance,
truthful capability details, and long-range AAPL Result/Replay reconciliation
are implemented on the local continuation. Final exact-head gates, remote
checks, native application acceptance, and whole-branch review remain.

## Latest local continuation

- PR #76 merged at `508d2d9e18befe2ebb1e80b9a6d8bf528c5c5def`; its required CI
  and CodeQL checks passed on the submitted head before merge. PR #80 and the
  earlier independent follow-ups remain included in `main`.
- R1–R3 are now in `main`: per-Result
  cross-process SQLite leases, retention pins before artifact visibility,
  atomic completed-bar persistence, committed-prefix recovery, and typed
  preservation notices. The exact Daily fill timestamp defect and the
  independent schema-2 hash oracle are also fixed in `main`.
- The synchronized `main` baseline at `508d2d9` built successfully with the
  pinned vcpkg toolchain and passed CTest 277/277. Eight frontend, Replay,
  Results, chart, and application-startup test suites were then repeated three
  times each; all 24 repeated runs passed. The Task 17 predecessor passed
  RunTest 295/295, Quality with 100% changed-line coverage, ASan/UBSan 249/249,
  and TSan 249/249. Before the current R7/R8 additions, commit `1e1c592`
  passed exact-head RunTest 321/321 and complete Quality with 98.0% diff
  coverage. R7/R8 focused tests pass; the final combined head still needs
  exact-head tests, Quality, and both sanitizer presets.
- Earlier PR creation through the connected GitHub integration failed with
  HTTP 403 (`Resource not accessible by integration`), and the local `gh` CLI
  was unavailable. Remote CI/CodeQL has not run for this continuation; retry
  publishing after the final local gates.
- The user-requested branch reconciliation was completed before this
  continuation. Work is on `codex/issue10-task18`, based on `4415373` with
  `508d2d9` in its `main` ancestry. Task 17/18 and provenance commits precede
  the current R7/R8 continuation. Local drafts and private documents remain
  preserved in their existing Git stashes.

Task 17's previous UTC data-day close condition remains implemented locally
through `bed5388`, with its preceding exact-head local gates recorded above.
Task 18's historical gain, extrema, persistent strict breakouts, bounded
immutable pre-range summaries, and v3 Strategy framing are implemented in the
current working tree. Source provenance is now explicit: `GetFromDb.py` writes
an exact-byte hash-bound timeframe manifest; the snapshot builder rejects
missing or mismatched declarations for seven-column input while retaining the
explicit `schemaName` format, `--include-provenance` column order, and
trailing-column compatibility. The manifest matches all 503 tracked extracted
CSVs against DuckDB exports, and a real AAPL/MSFT/TSLA snapshot build succeeded.
The source-manifest Python tests pass 4/4. R7's saved-Result UI workflow now
shows all six supported and six unavailable schema-2 capabilities and the
persisted dividend-accounting warning as plain text. R8 builds all 31,466 AAPL
source rows into an immutable snapshot, runs and saves the full 2018–2026
backtest, then checks all 31,466 saved Hourly frames and 2,014 UTC Daily frames;
known 2026-05-04/05 OHLCV values, fills, exact saved spans, and per-day portfolio
checkpoints are reconciled. These focused checks pass locally; final exact-head
Quality and sanitizer gates and remote CI/CodeQL remain pending. Native Replay
checks across window sizes, resizing, playback, zoom/pan and long-range data,
plus delivered-checkout `Launch.sh` acceptance, remain open. The native
interactive action was rejected by automatic review for a usage limit; no
workaround was attempted. Automated Qt workflows do not replace the requested
hands-on viewport and launcher acceptance.

## Already merged to main

These are the recent independent follow-ups; earlier PRs #68/#69/#70 remain
merged. This table does not imply that every basic Replay feature is absent
from main or that any broader source checkpoint is fully accepted.

| Delivery | Evidence and limit |
| --- | --- |
| Full-money half-even rounding | PR #71; exact 1/3/5-share and overflow cases. This closes the scoped R4 arithmetic repair, not general portfolio functionality |
| Initial delivery inventory | PR #72; preserves unfinished work rather than declaring the plan complete |
| Metric unit/meaning/example display and typed thresholds | PR #74; unused period identity/editability preserved. Historical predicates are not included |
| Real selectable multi-fill fixture | PR #75; five Buys/five Sells through real Engine/Store/Replay with literal OHLCV, fills, costs and portfolio checkpoints. Repeated hash equality is not an independent hash oracle |
| Tracked CSV refresh | PR #77; the local `0a17221` and `de9d128` data commits are patch-equivalent to the delivered tracked-data update |
| Fixed SHA-256 rotations | PR #78; merged as `4750042`, with behavior-preserving digest coverage |
| SQLite manifest-pin regression | PR #73 reopened and merged as `34b958d`; all exact-head required CI/CodeQL checks succeeded. No production dependency/version or runtime behavior changed |
| Validated snapshot reuse and bounded hashing | PR #80; merged as `dbcbd39`; required remote checks passed. This is completed-work delivery, not whole Issue #10 acceptance |

## Historical publications and local repairs

The checkpoints below preserve what was true at the time they were recorded;
they are not the current delivery state unless the latest summary above repeats
the item. The broad PR #76 work later merged at `508d2d9` after its required
checks passed.

- PR #73's independent two-file delivery at main-aligned `9b1c492` is now
  merged. Exact local full tests/Quality and fresh focused tests passed;
  current-head CI/CodeQL succeeded and remote main was verified at `34b958d`.
- Draft PR #76 previously published the broader implementation at `b7e27a9`. Exact local
  tests/Quality passed, but remote CI failed Linux test compilation, Linux IWYU,
  a macOS Results widget test and long-range Replay loading. It is not ready
  for main, even if subsequent mechanical gates become green.
- The follow-up repair replaces the remaining 47 narrowing Qt wait macros
  without changing their predicates or five-second limit, supplies the reported
  libstdc++ macro-provider header, and guards a transient empty table cell in
  rename testing. The crash reproduced locally with exit 139; the corrected
  widget flow and 20 repeated full Results test-target runs passed. This repair
  was committed locally as `32e5034` and not pushed at that failed-gate checkpoint.
  Exact RunTest passed 243/243
  (89.37 seconds). Complete non-fast Quality against main `34b958d` failed
  changed-line coverage: 1,995/2,038 lines (approximately 97.89%, below 98%).
  Static analysis completed and coverage tests passed 243/243 (88.96 seconds),
  but these partial successes do not clear the failed aggregate gate. Initial
  attempts also stopped at a missing shell VCPKG_ROOT and sandbox DNS; the
  complete rerun used the documented pinned checkout and approved networking.
  At that checkpoint, coverage repair, fresh passing exact-head gates and
  remote confirmation remained pending. The later `98819b3` repair is below;
  post-merge exact-head gates and remote acceptance still remain required.
- A subsequent test-only follow-up exercises actual inaccessible catalog UI,
  disabled actions, an empty table, and successful refresh after repairing the
  isolated temporary store path. The focused test passed; a temporary wrong
  error-message mutation made it fail and was fully removed. Independent
  code/docs/tests review found no new scoped P0–P2. At that focused-test
  checkpoint this did not yet prove a fresh aggregate coverage pass; the
  broad checkpoint's blockers remain unresolved.
- That follow-up was published as #76 head `98819b3` after exact RunTest
  passed 243/243 (87.99 seconds) and complete non-fast Quality against `34b958d`
  exited zero: clean coverage tests 243/243 (87.70 seconds), changed lines
  1,999/2,038 and branches 224/240. This supersedes the earlier failed local
  coverage checkpoint, not the original semantic blockers. CodeQL succeeded;
  required project CI did not start because the PR had an inventory merge
  conflict. A normal merge aligns with main `2a21657` (concurrent data-only #77),
  preserves main's data, and resolves compatible document intents. Fresh
  merge-head local gates and complete required CI were pending at that point.
- Published merge head `e45ac7b` then passed exact RunTest 243/243 (89.43
  seconds) and complete non-fast Quality against `2a21657`: clean coverage
  tests 243/243 (90.28 seconds), changed lines 1,999/2,038 and branches 224/240.
  Remote run `38538578246` passed standards, static analysis, Ubuntu/macOS
  tests and ASan/UBSan/TSan; CodeQL also passed. Coverage failed the unchanged
  five-second long-range load wait at 5,002 ms, before generating coverage.
- The existing `longRangeSeekPresentsEveryReachedCandleAndFill` test is the
  actual remote red regression. Local old-code runs pass, so local timing is
  comparative evidence, not a locally reproduced timeout. macOS 14.6 arm64,
  Apple LLVM 16, checked-in coverage configuration (`--coverage -O0 -g`),
  offscreen Qt 6.9.1, same synthetic 23,777-bar / 6,793-fill fixture, three
  sequential runs per implementation: load before 2,376/2,357/2,280 ms and
  after 1,727/1,698/1,760 ms; seek before 2,470/2,570/2,451 ms and after
  2,470/2,534/2,475 ms. Reproduce with
  `QT_QPA_PLATFORM=offscreen Output/coverage/Tests/bte_result_replay_frontend_tests longRangeSeekPresentsEveryReachedCandleAndFill`.
  CPU sampling identified canonical SHA-256 as dominant in both open/catalog
  workers. The local repair uses constrained fixed unsigned rotations, without
  changing public APIs, digests, cancellation, allocation, validation or waits.
  Independent literal binary/padding and million-a digest tests pass before
  and after: these characterize preserved behavior, not the performance RED.
  Incremental coverage counters from the changed test TU were stale and moved
  out of the build; these focused runs are not aggregate coverage acceptance.
  Fresh complete exact-head gates and remote confirmation are still required.
- The remaining five-second coverage failure was traced to Replay reading and
  validating the same exact snapshot selection twice: first in `openResult()`
  and then again in `ResultReplay::open()`. The maintainer approved a bounded
  `openResultWithData()` seam that preserves `openResult()` while returning the
  validated result and exact retained bars together. The implementation is
  currently uncommitted on the issue10 worktree. Exact-row, missing-segment,
  invalid-ID and cancellation cases pass; after the final internal policy
  refactor, the Bindings Replay suite passed 21/21, the ResultStore suite
  57/57, and the Replay frontend suite 20/20. The unchanged long-range test
  locally measured 563 ms load / 2,481 ms seek on macOS 14.6 arm64, Apple LLVM
  16, Qt 6.9.1, `Output/qt-dev`. `./RunTest.sh` passed 248/248 in 87.97 seconds
  before that last internal refactor and must be rerun. This is local evidence,
  not the Linux coverage runner. Exact-commit full tests/Quality, sanitizer,
  review and remote CI remain pending; this does not close the CI failure yet.

- Local follow-up on `codex/issue10-consolidated` is now committed through
  `90c6682` (`9c37029` avoids the duplicate snapshot open/read; `90c6682`
  fixes clang-tidy's const-move finding). Exact-head `./RunTest.sh` passed
  248/248 (87.71 seconds). Project standards/clang-format, full clang-tidy,
  cppcheck, IWYU, and scan-build passed (`No bugs found`). Coverage tests passed
  248/248 (90.64 seconds), diff coverage was 98%, and changed-branch coverage
  was 93.9%. `RunQuality.sh` itself stopped at coverage configuration because
  sandbox DNS could not resolve GitHub for GoogleTest FetchContent; the same
  coverage configure/build/test/report/check steps passed using the existing
  local GoogleTest source. ASan/UBSan/LSan passed 205/205 (53.62 seconds), and
  TSan passed 205/205 (79.99 seconds). These are macOS local results, not
  remote acceptance. The commits still need publication, independent review,
  and required remote CI. All R1–R3, R8, daily-gap, CSV provenance, historical
  predicate, capability, and native/real-data acceptance entries below remain
  open.

## Historical gates and follow-up checkpoints

Latest follow-up evidence (2026-10-15):

- Published #76 `d50dabb` passed full RunTest 245/245 (84.28 seconds) and
  complete non-fast Quality: clean coverage tests 245/245 (87.06 seconds),
  changed lines 2,010/2,049, branches 225/240. Remote run `38583994672` still
  failed the unchanged Linux long-load wait at 5,002 ms. Its macOS failure was
  Git checkout `early EOF`, before building/testing; only that job was requested
  to rerun. These are different failure classes, not a successful CI checkpoint.
- Rotation-only two-file PR #78 (`e4f9e0c`) excludes all unfinished persistence,
  chart and CSV behavior. Main baseline tests passed 219/219 (57.74 seconds);
  exact extraction RunTest passed 221/221 (58.57 seconds), complete non-fast
  Quality passed with clean coverage tests 221/221 (63.59 seconds), lines 11/11
  and no changed runtime branches (0/0). Local backend-only ASan/UBSan and TSan
  passed 184/184 (49.96 and 75.43 seconds). Every required remote check passed
  and the PR was squash-merged into `main` as `4750042` on 2026-10-15; this
  independently useful improvement does not fix #76's load RED.
- New sampling of `d50dabb` found 372 non-const and 59 const fixed-array `.at`
  top-of-stack samples, plus the already observed full-input padding copy and
  reallocation. A local bounded-array-only probe passed the independent digests
  and three original long-range runs: load 1,549/1,564/1,541 ms, seek
  2,491/2,487/2,475 ms. The next single-variable probe directly hashes complete
  input blocks and copies only the final padding into a 128-byte stack array:
  load 980/1,115/967 ms, seek 2,481/2,472/2,491 ms. The unchanged pre-array
  comparison samples and command/machine limits above still apply. These local
  sequential measurements are not remote acceptance or a latency guarantee.
- The follow-up at `083cf26` keeps fixed array indices bounded by an explicit
  round extent, changes no canonical framing or public API, and checks
  independent empty/abc, binary padding-boundary and million-a digests plus
  nonzero-offset borrowed byte/string views. Exact-head RunTest passed 246/246;
  full Quality passed against `4750042` (98% changed lines, 236/252 branches);
  ASan/UBSan and TSan each passed 203/203. Remote confirmation and independent
  follow-up review remain pending; the broad PR is not thereby accepted.
- After switching between baseline and broad branches, a diagnostic executable
  passed its Qt cases but exited 139 while merging stale GCDA counters. It is
  not counted as a passing execution. Only generated coverage counters were
  moved to an owned temporary directory; the clean-counter rerun passed.
  Final full Quality will rebuild fresh coverage; source/user artifacts were
  neither deleted nor altered for counter cleanup.

| Priority / plan area | What remains | Current state |
| --- | --- | --- |
| R1: live writer ownership | Final exact-head, remote and native acceptance | Implemented with per-Result SQLite leases; local child-process tests cover live empty writers, process death, committed prefixes and aliased paths |
| R2: retention before visibility | Final exact-head, remote and native acceptance | Implemented with pin-before-visibility and conservative reference preservation; local Results/fault/process tests pass |
| R3: execution-time durability | Final exact-head, remote and native acceptance | Implemented with per-bar atomic commits, atomic terminal tail, and committed-prefix recovery; local Engine/Store/process tests pass |
| Legacy staging compatibility | Final review and native workflow acceptance | Unleased Staging and sidecars stay byte-identical and produce typed English preservation notices; accessibility/refresh test passes |
| CI / Results rename acceptance | Verify current exact head | Prior GCC wait/IWYU/rename fixes are present; current Results workflow test and full local CTest pass |
| Exact-head CI readiness | Run final committed-head checks, publish a PR, and obtain required CI/CodeQL | Commit `1e1c592` passed RunTest 321/321 and Quality with 98.0% diff coverage. R7/R8 focused tests pass; their current changes still need full exact-head tests, Quality, ASan/UBSan, TSan, and remote CI/CodeQL |
| CI long-load responsiveness | Verify on current remote coverage runner | Snapshot reuse and bounded SHA work are present; previous local samples do not replace current exact-head remote coverage |
| R8: long-range saved-data completeness | Complete native visible-chart confirmation against exact saved bars and fills | Automated AAPL acceptance passes across all 31,466 source/snapshot/result Hourly bars and 2,014 UTC Daily candles; exact spans, 2026-05-04/05 OHLCV, fills, and daily portfolio checkpoints match. Native viewport acceptance remains open |
| T: exact Daily fill selection | Verify exact behavior against real saved data | Fixed locally by requiring exact selected timestamps; regression test covers an interior gap |
| L: seven-column source provenance | Distinguish actual hourly CSV from a seven-column daily export; preserve or explicitly decide trailing-column compatibility | Implemented locally with exact-byte hash-bound timeframe declarations; 503 tracked CSVs were byte-compared to DuckDB exports, C++ accepts/rejects and legacy trailing-column tests pass; exact-head gates remain |
| R6: independent hash oracle | Re-run on final exact submitted commit | Independent literal schema-2 hash oracle is implemented and passes locally |
| R7: truthful capabilities | Complete exact-head and native confirmation | Implemented in the Result details pane. Focused UI test verifies all persisted supported/unavailable families and the dividend-accounting warning as plain text; no unsupported values are synthesized |

## Requested features and final acceptance still unfinished

Task 17/18 and source provenance are implemented on the continuation branch.
The pre-R7/R8 head `1e1c592` passed local RunTest and Quality; current R7/R8
changes have focused tests but still need final combined-head tests, Quality,
ASan/UBSan, TSan, branch review, and remote CI/CodeQL.

| Request / plan task | Remaining work |
| --- | --- |
| Historical conditions, Task 18 | Implemented locally: 30 available UTC data-date close-to-close gain; prior 21 complete data-date highs/lows excluding the current date; persistent strict breakouts; bounded same-snapshot pre-range warm-up. Current final-head gates, remote checks, and native acceptance remain |
| Save Result / Save & Replay, Task 14 | R1–R3 and current local workflow tests are implemented; exact-head/remote checks and native restart/failure acceptance remain |
| Results view, naming, Trash/Restore and Open Replay, Tasks 15/25 | Current worker/error/refresh/restore tests pass; exercise the full native workflow and real saved data |
| Progressive Hourly/Daily replay, Tasks 21/24 | Automated deterministic and full-range AAPL tests pass for exact bars, dates, fills and portfolio checkpoints; native viewport behavior and readable long-range display remain |
| Trade glyphs, Tasks 22/26/28 | Source Buy/Sell direction, candle anchoring, eight-pixel wick clearance and Buy-only grouping have scoped evidence; final current-head exact-data and visual checks remain. Sell grouping is not approved |
| Viewport / zoom / Follow latest / Max | Automated viewport coverage passes; complete native three-size, wheel/pan, resize-during-playback, full-range and readable-tail checks |
| Exact real-data comparison / Tasks 10/11B/13 | Automated full-range AAPL source/snapshot/saved Result/Replay reconciliation passes; native visible-candle confirmation remains |
| Delivered Launch.sh path / Tasks 19/20/27 | Verify the final merged checkout builds/launches the exact code, exposes Save/Replay, and runs the selected long-range multi-fill configuration |
| Whole-branch review / Task 12 | Review all branch commits and current changes, run final exact-head tests/Quality/sanitizers, obtain CI/CodeQL, and complete native acceptance |
| Workspace and branches / Tasks 27/29 | Reconcile every intended hunk, preserved root draft and stash; only remove confirmed non-main branches after all unique intended code is merged or explicitly retained |

## Preserved work, not omitted delivery

- The old user branches were audited and removed after confirming their
  delivered changes are represented in `main`. The Task 18 continuation is on
  `codex/issue10-task18`, based on `4415373` with `508d2d9` as its `main`
  ancestry. Task 17/18 and source-manifest commits are local; commit `1e1c592`
  passed RunTest and Quality. Current R7/R8 and status-document changes are
  uncommitted and require the final exact-head gates before publication.
- R1–R3 persistence implementation and its owning documentation are merged in
  PR #76. Remote checks and native acceptance for the broader continuation
  remain open.
- Separate CSV and trusted-Replay extraction stashes remain intact. Other
  pre-existing strategy/CI stashes and private root documents were not staged,
  deleted or published.
- Source period disabling/normalization was deliberately excluded to preserve
  #74's accepted typed identity semantics; it is not a silently lost change.
- Local Issue #10 drafts and private documents are preserved in `stash@{0}`;
  earlier design and code stashes remain intact. Branch cleanup did not delete
  Result or market-data artifacts.
