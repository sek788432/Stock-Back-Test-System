# Issue 10 Remaining Work

**As of 2026-10-14: Needs changes.** This is a current delivery/acceptance
summary, not a replacement for the full dated
[implementation plan](../Issue10DeterministicReplayFixturePlan.md) or the
[source inventory](Issue10IncrementalDeliveryInventory.md).

The maintainer requests merging independently completed work while repairing
the broad checkpoint. Existing code, local verification, publication and merge
are different states. A passing build does not clear a known data-loss defect.

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
| SQLite manifest-pin regression | PR #73 reopened and merged as `34b958d`; all exact-head required CI/CodeQL checks succeeded. No production dependency/version or runtime behavior changed |

## Current publications and local repairs

- PR #73's independent two-file delivery at main-aligned `9b1c492` is now
  merged. Exact local full tests/Quality and fresh focused tests passed;
  current-head CI/CodeQL succeeded and remote main was verified at `34b958d`.
- Draft PR #76 publishes the broader implementation at `b7e27a9`. Exact local
  tests/Quality passed, but remote CI failed Linux test compilation, Linux IWYU,
  a macOS Results widget test and long-range Replay loading. It is not ready
  for main, even if subsequent mechanical gates become green.
- The current local repair replaces the remaining 47 narrowing Qt wait macros
  without changing their predicates or five-second limit, supplies the reported
  libstdc++ macro-provider header, and guards a transient empty table cell in
  rename testing. The crash reproduced locally with exit 139; the corrected
  widget flow and 20 repeated full Results test-target runs passed. This repair
  is committed locally as `32e5034`, not pushed. Exact RunTest passed 243/243
  (89.37 seconds). Complete non-fast Quality against main `34b958d` failed
  changed-line coverage: 1,995/2,038 lines (approximately 97.89%, below 98%).
  Static analysis completed and coverage tests passed 243/243 (88.96 seconds),
  but these partial successes do not clear the failed aggregate gate. Initial
  attempts also stopped at a missing shell VCPKG_ROOT and sandbox DNS; the
  complete rerun used the documented pinned checkout and approved networking.
  Coverage repair, fresh passing exact-head gates and remote confirmation remain.
- A subsequent test-only follow-up exercises actual inaccessible catalog UI,
  disabled actions, an empty table, and successful refresh after repairing the
  isolated temporary store path. The focused test passed; a temporary wrong
  error-message mutation made it fail and was fully removed. Independent
  code/docs/tests review found no new scoped P0–P2. This does not yet prove a
  fresh aggregate coverage pass or resolve the broad checkpoint's blockers.

## Required before the broad implementation can land

| Priority / plan area | What remains | Current state |
| --- | --- | --- |
| R1: live writer ownership | A second process must not recover, move or quarantine a live writer's Staging. Add real concurrent-process tests and safe writer lifetime | SQLite lease direction approved; detailed written-design proposal preserved locally, not approved/implemented |
| R2: retention before visibility | Pin exact immutable segments before Staging/recovered drafts become visible; keep pins through failure, Drafts, Results and Trash | Current initialization/recovery gaps remain; part of the approved safety direction |
| R3: execution-time durability | Atomically persist every completed bar and its full canonical batch; stop on storage failure; recover only a committed prefix as Interrupted Draft | Current execution still records at terminal time; implementation and crash/commit-boundary tests missing |
| Legacy staging compatibility | Preserve and warn about old unleased Staging rather than guessing that its process is dead | Explicitly approved policy; code and English warning delivery not implemented |
| CI / Results rename acceptance | Clear all GCC wait-macro, IWYU and macOS crash failures on the actual submitted SHA | Local portability/null-cell repairs exist; not yet submitted/remote-accepted |
| Local changed-line coverage | Close meaningful coverage gaps without removing assertions or lowering the 98% gate | Exact `32e5034` full Quality failed at 1,995/2,038 lines; no push or remote CI requested for this repair |
| CI long-load responsiveness | Diagnose and fix the Linux coverage load that remains “Loading result” beyond five seconds; verify long seek, Max and resizing without stalls | Diagnostic time/status added; no accepted performance fix and no weakened timeout. This is not R8 data-completeness acceptance |
| R8: long-range saved-data completeness | Compare selected range, exact saved spans, actual source rows and visible Hourly/Daily candles to explain the reported sparse chart | Final real-data correctness comparison remains open; a faster load or longer wait cannot close this finding |
| T: exact Daily fill selection | Do not show fills from an unselected interior-gap timestamp merely because it lies between the day's first/last selected bars | Defect corroborated; narrow extraction retained in a stash, repair/acceptance open |
| L: seven-column source provenance | Distinguish actual hourly CSV from a seven-column daily export; preserve or explicitly decide trailing-column compatibility | Explicit source-timeframe contract unresolved; compatibility-aware extraction retained in a stash |
| R6: independent hash oracle | Pin schema-2 framing with an independent literal/worked expected hash, not equality between two executions of the same implementation | Not implemented |
| R7: truthful capabilities | Exercise warnings and display unsupported financial families as unavailable, not fabricated zeros or a claim of a complete engine | Scope/UX/test acceptance still open |

## Requested features and final acceptance still unfinished

| Request / plan task | Remaining work |
| --- | --- |
| Previous data-day close change, Task 17 | Implement up/down percent from the previous available UTC date's final CSV close; this is not the existing previous-hour predicate |
| Historical conditions, Task 18 | Implement 30 available UTC data-date close-to-close gain; prior 21 complete data-date highs/lows excluding the current date; persistent strict breakouts and chronology-safe pre-range warm-up |
| Save Result / Save & Replay, Task 14 | Source implementation exists in #76, but its lifecycle cannot be accepted before R1–R3 and complete restart/failure/real-app checks |
| Results view, naming, Trash/Restore and Open Replay, Tasks 15/25 | Source implementation and earlier isolated checks exist; complete the current-head worker/error/refresh tests and actual native workflow without the known safety gaps |
| Progressive Hourly/Daily replay, Tasks 21/24 | Verify exact saved-range bars, no truncation, frame/date continuity, actual fill timing and raw ungrouped accounting through both display timeframes |
| Trade glyphs, Tasks 22/26/28 | Source Buy/Sell direction, candle anchoring, eight-pixel wick clearance and Buy-only grouping have scoped evidence; final current-head exact-data and visual checks remain. Sell grouping is not approved |
| Viewport / zoom / Follow latest / Max | Source fixes have scoped evidence; complete three window sizes, wheel/pan bounds, resize during playback, full reached range and stable readable tail without disappearing/compressed candles |
| Exact real-data comparison / Tasks 10/11B/13 | Reconcile the actual CSV/snapshot/result/visible OHLCV and every raw fill/portfolio value; synthetic fixture coverage does not replace final real-data acceptance |
| Delivered Launch.sh path / Tasks 19/20/27 | Verify the final merged checkout actually builds/launches that code, exposes Save/Replay, and runs the selected long-range multi-fill configuration |
| Whole-branch review / Task 12 | Close remaining P0–P2, run every applicable exact-head gate and native acceptance; no universal “bug-free” claim from partial tests |
| Workspace and branches / Tasks 27/29 | Reconcile every intended hunk, preserved root draft and stash; only remove confirmed non-main branches after all unique intended code is merged or explicitly retained |

## Preserved work, not omitted delivery

- Original source `codex/issue10-replay-ux` remains pinned at `6ac45ea` with
  its 82 commits and 64 changed paths. Consolidation used normal commits,
  not a force-push or public-history rewrite.
- The detailed persistence design and its owning-document edits are in a named
  recoverable stash while completed-only delivery proceeds. This is not an
  implementation or an approved written-spec stage.
- Separate CSV and trusted-Replay extraction stashes remain intact. Other
  pre-existing strategy/CI stashes and private root documents were not staged,
  deleted or published.
- Source period disabling/normalization was deliberately excluded to preserve
  #74's accepted typed identity semantics; it is not a silently lost change.
- No branch cleanup or broad result/market-data deletion has been performed.
