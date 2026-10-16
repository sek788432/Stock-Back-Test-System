# Issue 10 Delivery and Remaining Work

**Status as of 2026-10-16: Needs changes.** The historical plan checkboxes
do not establish acceptance of the later Save/Results, chart, historical-condition,
or persistence-safety requirements. This inventory distinguishes code preserved
on the consolidated branch from work accepted into main.

## Delivered to main

| Delivery | Evidence and scope |
| --- | --- |
| Incremental implementation workflow | PR #69; the workflow files on the older local branch are byte-identical to main |
| Full-money half-even rounding | PR #71; scoped R4 arithmetic repair |
| Delivery inventory | PR #72; preserves unfinished work without accepting it |
| Manifest-pin regression | PR #73; exact SQLite dependency regression |
| Indicator units and typed thresholds | PR #74; historical predicates are separate unfinished work |
| Real selectable multi-fill fixture | PR #75; five buys/five sells with literal OHLCV and accounting expectations |
| Tracked CSV refresh | PR #77; local commits `0a17221` and `de9d128` are patch-equivalent to the delivered data |
| Fixed SHA-256 rotations | PR #78, main commit `4750042`; narrower than complete Replay load acceptance |

## Preserved implementation and local repairs

Draft [PR #76](https://github.com/sek788432/Stock-Back-Test-System/pull/76)
contains the broader Issue 10 implementation. It is not accepted for merge:
known persistence-safety and data/Replay acceptance gaps remain. Remote head
`693b8ef` passed platform tests, static analysis and sanitizers but failed the
coverage job and aggregate merge gate. A green mechanical gate alone would
not clear those semantic defects.

The local consolidated branch reached `cc18ab8`, including `9c37029` and
`90c6682` for `openResultWithData()`, which returns validated result records and
exact retained bars together. The prior exact source revision passed 248/248
local tests, individual analyzers, coverage steps, ASan/UBSan/LSan and TSan.
Its aggregate Quality run previously stopped at dependency-download DNS;
the successful manually completed coverage steps are not an aggregate pass.
Fresh publication checks and remote confirmation remain separate evidence.

The focused main-based extraction is `41ee2cc`; its own local checks and remote
acceptance must be recorded independently of the broader branch's results.
It does not include #76's 23,777-bar frontend regression fixture or unfinished
Save/Draft lifecycle changes.

The detailed [persistence safety proposal](../Specs/ResultPersistenceSafetyDesign.md)
is committed documentation, not code. Its architectural direction was approved;
the detailed proposed interfaces and evidence requirements remain reviewable.
The original design stash remains preserved as well.

## Remaining implementation and acceptance

| Area | Work still required |
| --- | --- |
| R1: writer ownership | Implement SQLite cross-process writer leases and real competing-process tests so opening another app cannot recover or move a live writer |
| R2: data retention | Pin exact segments before recoverable Staging/import/recovery visibility; retain pins through failures, Drafts, Results and Trash |
| R3: execution durability | Atomically persist each completed bar's canonical batch before progress; stop on storage failure; recover only the committed prefix as Interrupted Draft |
| Legacy compatibility | Preserve unleased Staging and removal sidecars, and display an English recovery notice without automatic cleanup |
| R8: real long-range completeness | Compare selected range, CSV rows, snapshot spans, saved data and Hourly/Daily candles; account for every actual OHLCV row and raw fill |
| Daily interior gaps | Filter fills by exact selected timestamps, rather than including unselected timestamps between a day's first/last selected bars |
| Seven-column CSV provenance | Resolve and implement an explicit source-timeframe contract and trailing-column compatibility; the same columns alone do not prove Hourly provenance |
| Task 17 | Previous available UTC data-date final CSV close up/down percentage, distinct from previous-hour change |
| Task 18 | 30 available UTC data-date close-to-close return; prior 21 complete data-date extrema excluding today; drawdown and strict persistent breakouts; chronology-safe pre-range warm-up |
| R6: independent hash oracle | Add an independently worked literal schema-2 framing/hash expectation rather than comparing two runs of the same implementation |
| R7: truthful capabilities | Exercise warnings and unavailable financial families in UI; unsupported accounting/metrics must not appear as fabricated zeros |
| Save actions, Tasks 14/15/25 | Accept Save Result / Save & Replay, names, details, Trash/Restore and Open Replay only after safety repair and current-version restart/failure/native tests |
| Replay, Tasks 21/24 | Verify progressive exact-range Hourly/Daily candles, raw fill/accounting timing, seek, pause, speed and complete terminal timeline |
| Trade glyphs, Tasks 22/26/28 | Final exact-data/native verification of candle anchoring, eight-pixel wick clearance and one Buy marker per candle; Sell grouping is not approved |
| Viewport | Three window sizes, wheel/pan bounds, resize during playback, Follow latest, Max and long seek; readable candles must not disappear or compress into a cluster |
| Real-data/native acceptance | Reconcile actual CSV/snapshot/result OHLCV and every fill/portfolio checkpoint; synthetic tests do not replace this |
| Launch.sh, Tasks 19/20/27 | Build and launch the final delivered checkout, expose the correct Save/Replay actions and run the chosen long-range multi-fill configuration |
| CI and review, Tasks 12/30 | Publish repairs, verify every required check on the actual submitted SHA, close P0–P2 findings and satisfy the applicable Definition of Done |
| Workspace, Tasks 27/29 | Reconcile preserved branches and stashes; remove branches only after unique intended changes are merged or explicitly retained |

The accepted historical convention uses CSV-derived UTC dates with actual rows,
including extended hours. A date's final CSV close is its reference close;
this is not an official exchange-session close. Thirty days means 30 available
data dates. The extrema window defaults to 21 prior complete data dates and
excludes the current date; every strictly qualifying bar satisfies a breakout.

## Local artifacts that must not be silently published or deleted

- The original 82-commit source branch and the consolidated branch remain
  recoverable; the draft PR is not a claim that all source is accepted.
- CSV, trusted-Replay, safety-design and pre-existing strategy/CI stashes
  remain preserved until their intended changes are reconciled.
- The root's untracked older plan and manifest test are separate artifacts.
  The older test expects dependency metadata that differs from current main;
  copying it over the delivered regression would reintroduce stale assertions.
- Personal documents and temporary personal-data material are outside this
  implementation and must remain local.

No universal bug-free claim follows from passing automated tests. Only the
explicitly verified scope can be marked complete.
