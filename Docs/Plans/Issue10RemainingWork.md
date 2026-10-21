# Issue 10 Remaining Work

**Current status as of 2026-10-22 (pre-publication checkpoint):** PR #85 is
merged to `main` at
`2f6a831f91b952696dd99f0e3d520a6add49197b`. The accepted Issue 10 application
workflows and Replay controls have now been exercised in the native
application, including Task 11B's synthetic date-only run and the normal
`Launch.sh`-built application path. A 31,466-bar Hourly `max` playback reached
its final frame, and a subsequent native read showed the app Ready. The
combined automation call took over 122 seconds, so it is not a playback timing
benchmark. The earlier shell-only `no screens available` result describes the
headless launch context; the freshly built app was opened through a temporary
native `.app` wrapper around the same launcher-built executable and its UI was
verified. This is the current delivery/acceptance summary, not a replacement
for the full dated
[implementation plan](../Issue10DeterministicReplayFixturePlan.md) or the
[source inventory](Issue10IncrementalDeliveryInventory.md).

Replay chart/detail splitter behavior is now verified. A Qt regression sends a
press/move/release sequence to the splitter handle at 1440×1300 and confirms
that the chart grows while the details pane shrinks. At a shorter viewport,
both panes are already at their configured minimum sizes, so dragging cannot
move the handle. The native drag was unchanged at the available display size,
consistent with that minimum-size constraint; the compact view remains
vertically scrollable.

The native Results workflow exposed and then verified a keyboard navigation
repair. Previously, Tab/Shift+Tab wrapped within the table cells and trapped
focus there. The Results table now leaves Tab navigation to the surrounding
actions while arrow keys move through rows. The rebuilt app was operated with
the keyboard from the table to View Details and Open Replay; the exact Result
ID and first replay frame were visible. A focused regression failed before the
fix and passes afterward.

The final local verification checkpoint is commit
`cbbefacdb4c44292619358b9df9a81a0474ea8ff`, based directly on the merged
baseline above. `./RunTest.sh` passed 333/333. The full non-fast
`./RunQuality.sh --base origin/main --head HEAD` exited 0: project standards,
clang-format, clang-tidy (29/29 files), cppcheck, IWYU, scan-build, and all
333 coverage tests passed; changed-line coverage was 1/1 (100%). The isolated
ASan/UBSan and TSan suites each passed 285/285. Independent code, documentation,
and test reviews found no remaining P0–P2 issue in this follow-up. These gates
are on the implementation commit; the documentation-only acceptance update
and exact PR head still require their own final verification and remote CI.

GitHub Issue #10 itself is narrowly scoped to the offline deterministic Replay
fixture. Its committed fixture and both named snapshot-determinism tests are
present, and GitHub closed the issue as completed on 2026-10-20. This report
tracks the broader Issue 10 delivery plan; those broader acceptance items are
separate from the closed issue.

PR #76 is merged, including R1–R3 persistence safety and the previously
published Replay repairs. PR #81 is also merged as `c7e487e`; Task 17/18,
seven-column CSV timeframe provenance, truthful capability details, and
long-range AAPL Result/Replay reconciliation are now in `main`. Its required
CI and CodeQL checks passed on the exact submitted head. PRs #82–#85 then
merged the review repairs and acceptance records. Native UI workflow checks
were performed on 2026-10-21 through a temporary application wrapper around
the actual `Launch.sh`-built app. The independent
review of PR #81 found a dotted-ticker CSV
filename mismatch and a weak non-empty-fill assertion; the follow-up branch
contained fixes and regression coverage for both, which were squash-merged in
PR #82 as `c6809d9` after every required remote check passed on GitHub's merge
candidate (submitted head `f950286` merged into base `c7e487e`). The submitted
head also passed local RunTest 333/333 and RunQuality with 100% changed-line
and 22/22 changed-branch coverage. Independent implementation, documentation,
and test reviews found no further code or test issues.

## Latest merged delivery

The current merged baseline is PR #85 at `2f6a831`; PR #85's required checks
passed. The following PR entries preserve their individual delivery history.

- PR #76 merged at `508d2d9e18befe2ebb1e80b9a6d8bf528c5c5def`; its required CI
  and CodeQL checks passed on the submitted head before merge. PR #80 and the
  earlier independent follow-ups remain included in `main`.
- PR #81 merged at `c7e487ee9034d54ed3cf61eca765c368276eb8a8` from submitted
  head `1591427ee61d88a661f85ebd98d99c0aa10c6ef1`. Its merge gate, Ubuntu and
  macOS tests, project standards, static analysis, changed-code coverage,
  ASan/UBSan, TSan, and CodeQL checks all completed successfully on that exact
  head.
- PR #82 merged at `c6809d9397e0425b186fe05a45b2464695e592cc` from submitted
  head `f950286544895b820bc8b6d9de37a2ae6375c84a`. Its required remote checks
  passed on GitHub's merge candidate (the submitted head merged into base
  `c7e487e`); exact-head local RunTest passed 333/333 and RunQuality passed
  with 100% changed-line and 22/22 changed-branch coverage. The merge-candidate
  Ubuntu/macOS suites each passed 333/333, and ASan/UBSan and TSan each passed
  285/285. It fixes dotted-symbol source resolution,
  strengthens saved-fill replay regression coverage, and reconciles the R1–R3
  decision status. Native UI workflow checks were subsequently performed in an
  isolated temporary application wrapper; direct launcher startup and fault
  injection coverage is automated as listed below.
- PR #83 merged at `8738c4e873692acdc8169a81e8ae10f48176d984`. It records the
  native acceptance results and remaining environment-bound checks in this
  ledger; all required CI checks passed on the docs-only change.
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
  coverage. Current continuation RunTest passes 322/322; complete Quality
  passes with 98.0% diff line coverage and 96.9% changed-branch coverage;
  ASan/UBSan and TSan each pass 274/274. These local results complement the
  successful remote CI and CodeQL checks above.
- At the post-PR #82 checkpoint, local `main` was fast-forwarded to `c6809d9`
  and verified against `origin/main`; merged personal branches with no unique
  work were removed locally and from GitHub. PR #83 later advanced `main` to
  `8738c4e`. This final-audit branch was created from that synchronized
  baseline; no earlier unmerged source changes were carried forward.
- All six pre-existing Git stashes remain intact, including the private Issue
  10 drafts and the persistence, CSV, trusted-Replay, strategy, and CI work.

Tasks 17/18, source provenance, R7 and R8 are merged. The native workflow
evidence and long-range data reconciliation are recorded in the table below.
The fresh `Launch.sh` build was opened through a temporary native `.app` wrapper
and its provenance and Result workflow were verified; the shell-only
`no screens available` result describes the headless command context. The
splitter drag and Results keyboard-navigation regressions are now reproduced,
fixed or verified, and covered by Qt tests. Final full gates and independent
review for this follow-up are tracked in the implementation plan and its PR.

## Already merged to main

This table records historical independent follow-ups; the current baseline and
native acceptance status are summarized above.

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
  remote confirmation remained pending at this checkpoint. The later
  `98819b3` repair is below; PR #81 and PR #82 subsequently passed their
  exact-head gates and required remote acceptance checks.
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
| R1: live writer ownership | None; crash and concurrent-writer cases are exercised by child-process tests | Implemented with per-Result SQLite leases; local child-process tests cover live empty writers, process death, committed prefixes and aliased paths; native save/relaunch lifecycle passed |
| R2: retention before visibility | None; failure behavior is covered by Results/fault/process tests | Implemented with pin-before-visibility and conservative reference preservation; native Results refresh/details/rename/Trash/Restore/Open Replay passed |
| R3: execution-time durability | None; mid-run failure recovery is covered by Engine/Store/process tests | Implemented with per-bar atomic commits, atomic terminal tail, and committed-prefix recovery; native save/relaunch passed |
| Legacy staging compatibility | None for the accepted behavior; a separate native fault-injection flow is not exposed | The registered Replay widget test preserves exact legacy Staging bytes and shows the typed notice beside a valid Result; sidecar preservation is covered by persistence tests |
| CI / Results rename acceptance | None for the exercised native workflow | Results rename and surrounding lifecycle controls passed; prior GCC wait/IWYU/rename fixes are present and the local workflow tests pass |
| Exact-head CI readiness | None | PR #82's required CI and CodeQL passed on its merge candidate; PR #83's required CI passed; local `main` is `8738c4e` |
| CI long-load responsiveness | None for the submitted revision | Snapshot reuse and bounded SHA work are present; required remote coverage passed on PR #81's exact head |
| R8: long-range saved-data completeness | None for tested views | Native long-range replay reached the exact saved tail; automated AAPL acceptance checks all 31,466 Hourly bars and 2,014 UTC Daily candles, exact spans, 2026-05-04/05 OHLCV, fills, and daily portfolio checkpoints |
| T: exact Daily fill selection | None | Fixed by requiring exact selected timestamps; regression test covers an interior gap and real saved data |
| L: seven-column source provenance | None for the submitted revision | Implemented with exact-byte hash-bound timeframe declarations; 503 tracked CSVs were byte-compared to DuckDB exports, compatibility tests pass, and required remote checks passed |
| R6: independent hash oracle | None | Independent literal schema-2 hash oracle is implemented and passed locally and in the required PR checks |
| R7: truthful capabilities | None | Native Result Details showed Completed/Available status, exact final equity/PnL, and capability details; UI tests verify persisted supported/unavailable families and the dividend-accounting warning |

## Native acceptance status and remaining items

Task 17/18 and source provenance are merged in `main` as PR #81 (`c7e487e`).
PR #82 (`c6809d9`) contains the independent review follow-ups, and PR #83
(`8738c4e`) records their native acceptance. Exact-head local checks passed,
and the required remote checks passed on GitHub's merge candidate. On
2026-10-21, I exercised the delivered
application through macOS native UI using a temporary `.app` wrapper around
the freshly built application and an isolated `BTE_DATA_STORE`. The direct
`Launch.sh` build completed and produced a real development snapshot, but its
shell launch exited before opening a window with `no screens available`; the
wrapper allowed native acceptance without changing the regular app or data
store.

The native session covered selectable Backtest strategies, standalone Save,
Save & Replay, relaunch persistence, Results refresh/details/rename/Trash/
Restore/Open Replay, Hourly and Daily UTC replay, step back/forward, play/pause
at 1x/5x/10x/max, seeking, zoom buttons and wheel zoom, drag pan, Follow latest,
three window sizes, and resizing during 5x playback. The saved long-range run
contained 31,466 Hourly bars and 434 fills; its 2,014-bar Daily replay reached
the same final equity ($100,609.07) and PnL ($609.07) as the Result. A Task 17
prior-data-day-close strategy produced a 10-share Buy at
`2018-05-02T18:00:00Z` on the 48-bar fixture. A Task 18 30-data-day gain
strategy completed with no match and unchanged cash. The other four Task 18
conditions were then run individually over all 31,466 Hourly bars with a
21-data-day lookback: 1% down from period high completed at equity $99,110.83
with an open 10-share position; 1% up from period low completed at equity
$99,300.18 with an open 10-share position; strict break above period high
completed at equity $101,727.36 flat; strict break below period low completed
at equity $100,674.52 flat. Each run showed at least one fill. The common sell
condition was previous data-day close down at least 1%. Two older incomplete
Results whose release manifests are unavailable were left intact; their status
remained Incomplete/Unavailable.

The repeated widget matrix recorded earlier remains 33/33 target runs. In this
audit, the focused max-batch test passed 3/3 QtTest cases, and the 23,777-bar
long-range seek test passed 3/3 cases (Replay load 553 ms; seek/presentation
2,321 ms on this machine). These are automated widget results, separate from
the native observations above.

A follow-up native automation request selected `max`, sought the 31,466-bar
Hourly Result to its first frame, and clicked Play. The combined request did
not return after 122 seconds. A later app binding showed the same Result at
bar 31,466/31,466 with status Ready, and a fresh accessibility read succeeded.
This verifies that playback reached the final frame and the app responded
afterward; the initial delay has no timing instrumentation, so it is not a
throughput benchmark and does not identify whether the delay came from playback
load or the accessibility bridge. No product-code fix is claimed.

Workspace reconciliation advanced with PR #85: local `main` and
`origin/main` both point to `2f6a831f91b952696dd99f0e3d520a6add49197b`.
GitHub currently lists only `main` as a remote branch. The local
`codex/issue10-final-acceptance` branch contains this final acceptance audit;
it is to be removed after the follow-up documentation/verification PR is
merged. The six pre-existing stashes remain intact.

| Request / plan task | Native evidence | Remaining work |
| --- | --- | --- |
| Historical conditions, Tasks 17/18 | Task 17 prior-data-day close generated a Buy; all five Task 18 predicates were run natively, with the four 21-day conditions tested over 31,466 bars; the 30-day gain run completed with no match. UTC semantics/defaults inspected | None for tested predicates; automated boundary/signal tests also pass |
| Post-merge review follow-up | Dotted-ticker source resolution and persisted-fill regression are merged in PR #82 (`c6809d9`); exact-head local RunTest passed 333/333 and RunQuality had 100% changed-line plus 22/22 changed-branch coverage; required remote checks passed on the merge candidate | None |
| Save Result / Save & Replay, Task 14 | Standalone Save and Save & Replay succeeded; saved long Result survived relaunch and opened Replay | None; injected persistence failure and restart recovery are covered by automated process/fault tests, not a user-facing UI control |
| Results view, naming, Trash/Restore and Open Replay, Tasks 15/25 | Refreshed, inspected, renamed, opened Replay, trashed and restored a saved Result; old incomplete/unavailable Results remained intact. Native keyboard-only traversal selected a row, reached View Details, and opened the exact ID in Replay | None for the tested lifecycle and keyboard path |
| Legacy staging compatibility | Existing incomplete Results were preserved; the registered Replay widget test exercises a valid Result beside byte-identical legacy Staging and a typed notice; sidecar retention is covered by persistence tests | None for the specified behavior; no separate native fault-injection UI is available |
| Progressive Hourly/Daily replay, Tasks 21/24 | Native Replay displayed 31,466 Hourly bars and 2,014 Daily bars; seek/tail and final portfolio values matched | None for tested long-range views; automated checks cover every saved bar |
| Trade glyphs, Tasks 22/26/28 | Buy/Sell markers were visually distinct from candle wicks; chart geometry tests assert the 8.0 logical-pixel Buy/Sell clearance and resize boundary | None. Buy grouping and ungrouped Sell leader behavior match the accepted chart design |
| Viewport / zoom / Follow latest / Max | Tested at 2310×1416, 2602×1552 and 3420×2018; wheel zoom, drag pan, Follow latest, resize during 5x playback, max-speed Daily playback to end, and 31,466-bar Hourly max playback to its final frame | None for control behavior. The initial combined automation request exceeded 122 seconds, so no playback throughput benchmark is claimed |
| Chart/details splitter / Tasks 10/24 | The Qt handle press/move/release regression passes at 1440×1300 and confirms the chart grows while the details pane shrinks. At compact height, both children are at minimum size and the handle correctly cannot move; the native app remains vertically scrollable | None; resize is accepted when the viewport has room, and the minimum-size behavior is covered |
| Exact real-data comparison / Tasks 10/11B/13 | Native initial/fill/tail states and final values matched; automated saved-data reconciliation covers every Hourly and Daily frame, OHLCV, fills, and portfolio checkpoints | None for the audited AAPL data; native visual inspection was performed at several positions and sizes |
| Delivered `Launch.sh` path / Tasks 19/20/27 | The launcher built the application and isolated snapshot; the native `.app` wrapper opened that exact fresh build. Build Information showed the checkout revision and executable SHA. A real AAPL Backtest saved Result `16d1eb795888a4090f6bd746a631195d`, and Replay stepped to its Buy fill at 2/48 with the matching portfolio. | None for the tested launcher workflow. A raw shell invocation in the display-restricted context reports `no screens available`; this is an environment limitation, not missing GUI acceptance. |
| Task 11B synthetic date-only Backtest→Replay | Fresh isolated AAPL snapshot with before/after UTC sentinels; Start=End=2024-01-02 selected 11/13 rows. Native Selectable Conditions run produced 5 Buy and 5 Sell fills, final position 0, cash/equity $9,999 and PnL ($1). Step/Back, terminal seek, Hourly/Daily, playback result switching, chart zoom in/out, and ten per-fill rows were inspected. Two distinct Result IDs had the same canonical hash `fdef1b01c82096602cbde5862973767e6c7d4792e07ad365f68d7ad9c43e6bb1`. | None for the specified synthetic workflow; no issue reproduced. |
| R1–R3 failure recovery | Native save/restart lifecycle succeeded; automated cross-process fault tests cover leases, atomic commits, and committed-prefix recovery | None for the specified recovery contracts; native failure injection is not an application control |

## Preserved work, not omitted delivery

- PR #81 delivered Task 17/18, source-manifest, R7 and R8 changes to `main`.
  PR #82 delivered the reviewed follow-up fixes; PR #83 recorded the native
  acceptance results and workspace reconciliation.
- R1–R3 persistence implementation and its owning documentation are merged in
  PR #76. PR #81's remote checks passed, and native save/restart and preservation
  behavior was exercised; native fault injection remains automated-only as
  listed above.
- Separate CSV and trusted-Replay extraction stashes remain intact. Other
  pre-existing strategy/CI stashes and private root documents were not staged,
  deleted or published.
- Source period disabling/normalization was deliberately excluded to preserve
  #74's accepted typed identity semantics; it is not a silently lost change.
- Local Issue #10 drafts and private documents, earlier design/code stashes,
  and all Result or market-data artifacts remain preserved.
