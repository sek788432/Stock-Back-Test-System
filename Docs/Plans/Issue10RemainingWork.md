# Issue 10 Remaining Work

**As of 2026-10-19: Needs changes.** This is a current delivery/acceptance
summary, not a replacement for the full dated
[implementation plan](../Issue10DeterministicReplayFixturePlan.md) or the
[source inventory](Issue10IncrementalDeliveryInventory.md).

GitHub Issue #10 itself is narrowly scoped to the offline deterministic Replay
fixture. Its committed fixture and both named snapshot-determinism tests are
present; this report tracks the broader Issue 10 delivery plan. The issue is
left open for the maintainer to close.

The maintainer requests merging independently completed work while repairing
the broad checkpoint. Existing code, local verification, publication and merge
are different states. A passing build does not clear a known data-loss defect.

## Latest local continuation

- PR #80, the completed snapshot-reuse and bounded-hash delivery, is merged to
  `main` as `dbcbd39d0a23243cd8ff1b832a71b9ec62d02fe6`; all required remote
  checks passed before merge.
- The Issue #10 consolidation worktree now implements R1–R3: per-Result
  cross-process SQLite leases, retention pins before artifact visibility,
  atomic completed-bar persistence, committed-prefix recovery, and typed
  preservation notices. The exact Daily fill timestamp defect and the
  independent schema-2 hash oracle are also fixed in this local worktree.
- After aligning R1–R3 with the latest `origin/main`, `RunTest.sh` passed
  270/270, including real child-process recovery, cancellation after opening a
  snapshot reader, and both exclusive-boundary Replay cases. Exact committed
  HEAD `RunQuality.sh`, ASan/TSan, remote PR checks and the actual native
  application flow remain pending.

The remaining Issue #10 acceptance items are: Task 17/18 historical
predicates and warm-up; explicit source timeframe/provenance and trailing
column compatibility for seven-column CSV; truthful unsupported capability
display; a real-data source-to-snapshot-to-saved-result-to-visible-candle and
fill comparison; visual Replay checks across window sizes, resize, playback,
zoom, pan and long-range data; final full-branch review and exact-head local
and remote gates; and delivered-checkout `Launch.sh` acceptance. The native
application check has not run because the computer-use action was rejected by
automatic review for a usage limit; no workaround was attempted.

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

## Current publications and local repairs

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

## Required before the broad implementation can land

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
| Exact-head CI readiness | Run exact submitted commit and obtain required PR checks | The current working tree passes 268/268 CTest tests; `RunTest.sh`, full Quality, sanitizers and remote checks for the final commit remain pending |
| CI long-load responsiveness | Verify on current remote coverage runner | Snapshot reuse and bounded SHA work are present; previous local samples do not replace current exact-head remote coverage |
| R8: long-range saved-data completeness | Compare selected range, exact saved spans, actual source rows and visible Hourly/Daily candles to explain the reported sparse chart | Final real-data correctness comparison remains open; a faster load or longer wait cannot close this finding |
| T: exact Daily fill selection | Verify exact behavior against real saved data | Fixed locally by requiring exact selected timestamps; regression test covers an interior gap |
| L: seven-column source provenance | Distinguish actual hourly CSV from a seven-column daily export; preserve or explicitly decide trailing-column compatibility | Explicit source-timeframe contract unresolved; compatibility-aware extraction retained in a stash |
| R6: independent hash oracle | Re-run on final exact submitted commit | Independent literal schema-2 hash oracle is implemented and passes locally |
| R7: truthful capabilities | Exercise warnings and display unsupported financial families as unavailable, not fabricated zeros or a claim of a complete engine | Scope/UX/test acceptance still open |

## Requested features and final acceptance still unfinished

| Request / plan task | Remaining work |
| --- | --- |
| Previous data-day close change, Task 17 | Implement up/down percent from the previous available UTC date's final CSV close; this is not the existing previous-hour predicate |
| Historical conditions, Task 18 | Implement 30 available UTC data-date close-to-close gain; prior 21 complete data-date highs/lows excluding the current date; persistent strict breakouts and chronology-safe pre-range warm-up |
| Save Result / Save & Replay, Task 14 | R1–R3 and current local workflow tests are implemented; exact-head/remote checks and native restart/failure acceptance remain |
| Results view, naming, Trash/Restore and Open Replay, Tasks 15/25 | Current worker/error/refresh/restore tests pass; exercise the full native workflow and real saved data |
| Progressive Hourly/Daily replay, Tasks 21/24 | Automated deterministic behavior passes; verify exact real saved-range bars, date continuity, fill timing and raw ungrouped accounting through both display timeframes |
| Trade glyphs, Tasks 22/26/28 | Source Buy/Sell direction, candle anchoring, eight-pixel wick clearance and Buy-only grouping have scoped evidence; final current-head exact-data and visual checks remain. Sell grouping is not approved |
| Viewport / zoom / Follow latest / Max | Automated viewport coverage passes; complete native three-size, wheel/pan, resize-during-playback, full-range and readable-tail checks |
| Exact real-data comparison / Tasks 10/11B/13 | Reconcile the actual CSV/snapshot/result/visible OHLCV and every raw fill/portfolio value; synthetic fixture coverage does not replace final real-data acceptance |
| Delivered Launch.sh path / Tasks 19/20/27 | Verify the final merged checkout builds/launches the exact code, exposes Save/Replay, and runs the selected long-range multi-fill configuration |
| Whole-branch review / Task 12 | Close remaining P0–P2, run every applicable exact-head gate and native acceptance; no universal “bug-free” claim from partial tests |
| Workspace and branches / Tasks 27/29 | Reconcile every intended hunk, preserved root draft and stash; only remove confirmed non-main branches after all unique intended code is merged or explicitly retained |

## Preserved work, not omitted delivery

- Original source `codex/issue10-replay-ux` remains pinned at `6ac45ea` with
  its 82 commits and 64 changed paths. Consolidation used normal commits,
  not a force-push or public-history rewrite.
- The detailed persistence design and owning-document edits are now present in
  the Issue #10 consolidation worktree; code is implemented, while exact-head
  gates and native acceptance remain open.
- Separate CSV and trusted-Replay extraction stashes remain intact. Other
  pre-existing strategy/CI stashes and private root documents were not staged,
  deleted or published.
- Source period disabling/normalization was deliberately excluded to preserve
  #74's accepted typed identity semantics; it is not a silently lost change.
- No branch cleanup or broad result/market-data deletion has been performed.
