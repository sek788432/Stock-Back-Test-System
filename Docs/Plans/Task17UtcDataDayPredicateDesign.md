# Task 17 UTC Data-Day Predicate Design

**Status:** Approved for implementation

**Scope:** Previous UTC data-day close up/down percentage predicate only. Task
18 remains a separate design and implementation slice.

**Semantic approval:** Approved by the maintainer on 2026-10-20.

**Implementation status:** In progress on `codex/issue10-remaining`; the bounded snapshot lookup and its contract tests are implemented locally. Strategy, Engine/Bindings, UI, full specification updates, and exact-head gates remain.

## Problem

Selectable Conditions can compare a bar with the prior actual bar, but cannot
compare it with the last source close from the previous UTC data day. The
existing `closeChangePercent` source must keep its current behavior and saved
Strategy hash. Task 17 needs a distinct source that reads only the immutable
CSV-derived snapshot used by the run, including the source's extended-hours
rows, while keeping pre-range history out of the executed result.

## Accepted behavior

- A UTC data day is a UTC date containing at least one source row. A date with
  no rows does not count; no row or close is synthesized.
- A data day's close is the close of its last chronological source row,
  including extended-hours rows. This is the CSV policy, not an official
  exchange close or exchange calendar.
- For each current data day, compare the current actual bar's close with the
  previous available data day's close. Exclude the entire current UTC date
  from historical references, even if the run begins partway through that
  date.
- The UI accepts a positive magnitude and a separate Down or Up direction.
  Down and Up use inclusive at-least thresholds. A missing or zero reference
  is absent and does not match. The condition never looks forward.
- Existing prior-actual-bar percentage behavior and hashes remain unchanged.
- Pre-range history does not add engine callbacks, indicators, orders, fills,
  portfolio records, or Replay candles.

## Proposed architecture

Keep source selection in the Data/Bindings boundary and keep Strategy unaware
of files, CSV parsing, or mutable source paths.

1. Extend `ReleaseSnapshotReader` with a query for the last source row before a
   supplied UTC date boundary for one symbol. The query uses its already-opened
   immutable snapshot and the symbol's timestamp-ordered segment rows. It
   returns at most one `SnapshotBar`, the last row from the previous available
   UTC data day. It does not use a calendar-day lookback limit, so a long gap in
   source dates still finds the previous available data day. If none exists,
   it returns an absent value.
2. Extend `SnapshotSelection` with a distinct optional pre-range history
   value. `select()` continues to return only the requested execution bars in
   `.bars`; history never gets appended to that vector. The snapshot identity
   remains the existing `DataSelectionIdentity`, whose `snapshotId` identifies
   the immutable source used for both selection and history. The binding fills
   the optional history value from the reader query in step 3.
3. In `runPersistedBacktestConfiguration`, derive the UTC date boundary from
   the first selected execution bar and request its preceding data-day close
   from the same `ReleaseSnapshotReader`, then attach it to the
   `SnapshotSelection` history field. Convert the close through the same
   nanodollar-to-dollar conversion used for execution bars. Pass the bounded
   value as an optional Strategy history context through `BacktestRequest`.
   The production Backtest page selects whole UTC dates, so its in-range rows
   contain each selected data day's source rows. Direct/in-memory Engine
   callers without snapshot history get an absent first-date reference and
   never trigger a CSV or download fallback. The reader query is tied to the
   selected `snapshotId`.
4. Add a Task 17 source to `ConditionSource` and allow `SelectableStrategy`
   construction to receive the optional history context. The strategy keeps
   the seeded previous data-day timestamp/close and the last close observed
   for the current UTC data day. It evaluates the new condition before
   updating the current day's close. On a UTC date transition, the last
   observed close of the prior in-range date becomes the reference. Dates with
   no bars naturally do not appear in this state. The history context seeds
   only this price-reference state; it does not feed indicators or produce a
   signal.
5. Keep order timing in `BacktestExecution`: the signal from an actual bar
   still schedules an order for the next actual bar's open. Result persistence
   and Replay continue to consume the actual selected bars only.

The proposed Strategy history value is an optional value object containing
the timestamp and close of the last source row before the first executed UTC
date. It is immutable after Strategy construction. The Strategy derives the
UTC date from `core::Timestamp` (`sys_time<milliseconds>`) using UTC day
flooring; local timezone and exchange DST do not participate.

`ReleaseSnapshotReader::lastCloseBeforeUtcDate()` requires an exact UTC
midnight boundary. A non-midnight value is rejected as `invalidArgument` so
same-date rows cannot accidentally enter the prior-data-day reference.

### Alternatives considered

- **Feed pre-range bars through `onBar`:** rejected. It can advance indicators,
  evaluate signals, or accidentally leak warm-up bars into execution and
  persistence. A special warm-up callback would add a second state transition
  to keep equivalent and test.
- **Let Strategy query the snapshot or CSV:** rejected. It would couple the
  Strategy module to storage and mutable I/O, weaken deterministic execution,
  and bypass the current immutable selection boundary.
- **Use only actual in-range bars and roll their closes:** insufficient for
  the first executed date, whose previous available data day may be outside
  the configured range. The one-row snapshot context seeds that first date;
  actual chronological bars advance state for later dates.

## Predicate representation and UI

Add a distinct `ConditionSource` for change from the previous UTC data-day
close. Retain the existing `Comparison` and percentage numeric domain. Compile
the visible positive magnitude plus direction into the corresponding signed
inclusive comparison (`current change <= -X` for Down; `>= +X` for Up). The
new source computes `(currentClose / priorDataDayClose - 1) * 100` from the
current actual bar and seeded/rolled reference. Keep `closeChangePercent`
unchanged and separately selectable.

The Backtest editor must label the metric as change from the previous
**CSV/UTC data-day close** and explain that the close is the last source close,
including extended hours. Direction choices are **Down** and **Up**; the
entered threshold remains a positive percentage magnitude. Existing comparison
controls must not present an ambiguous second direction for this source. If the
editor's current row model cannot represent this without changing legacy
behavior, extend the row model narrowly for this source.

## Strategy identity compatibility

`canonicalStrategyHash` currently frames every Strategy with
`bte-strategy-v1` and selectable plans with `selectable-conditions-v1`.
Preserve byte-for-byte output for the starter Strategy and every existing plan
that does not contain the new source. When a plan contains the Task 17 source,
use `bte-strategy-v2` and `selectable-conditions-v2` framing. Include the
source, signed comparison, threshold bits, numeric domain, and condition order
using the established canonical field encoding. No preexisting plan is
reinterpreted, and changing the new source, direction, threshold, group logic,
or condition order changes its v2 hash.

The result continues to identify the immutable snapshot through the existing
`DataSelectionIdentity.snapshotId` and execution spans. The bounded history
query is deterministic from that same immutable snapshot and the first
executed UTC date; it does not require new persisted warm-up bars or alter the
Replay data selection. The stored v2 Strategy hash identifies the predicate
semantics, while the stored snapshot identity identifies its source data.

## Failure and boundary behavior

- No preceding source row: the first executed UTC data day's new condition is
  absent/nonmatching. Do not fail the whole Backtest and do not consult the
  mutable CSV, even if that CSV has since gained older rows.
- Zero or invalid prior close: absent/nonmatching; never divide by zero.
- Invalid or nonchronological actual bars: preserve existing request/strategy
  validation behavior; do not silently sort the engine's execution stream.
- Multiple bars in one UTC date: all use the same previous-data-day close.
- At the next UTC data date, use the prior actual data date's final observed
  source close, including its extended-hours rows.
- Missing UTC dates and Friday-to-Monday transitions do not create synthetic
  data days. UTC midnight, year boundaries, leap days, and exchange DST do not
  change the grouping rule.
- Snapshot history query errors propagate as normal data-selection errors;
  absence is distinct from a storage/read failure.
- A snapshot history query boundary that is not UTC midnight is rejected as
  `invalidArgument` rather than risking a same-date lookback.

## Required implementation and evidence

Implement test-first across the existing Strategy, Engine, Snapshot/Bindings,
and Qt Backtest seams. Tests must establish:

1. Strategy unit literals for a prior close of 100: Down 5% matches 95 exactly;
   Up 5% matches 105 exactly; adjacent values on either side do not match;
   zero/absent history does not match. Buy and sell groups both work.
2. Prior-data-day close is the final chronological row including extended
   hours: regular close 100, later extended close 110, next date 104.5 is
   exactly 5% Down and 115.5 exactly 5% Up. Multiple bars on the next date
   share 110 as their reference.
3. Friday-to-Monday, an empty intervening date, UTC midnight, year/leap
   boundaries, and exchange-DST dates use UTC data-date grouping.
4. The reader returns at most one pre-range row from the same snapshot, before
   the first executed UTC date; a row earlier on the first executed date is
   excluded. A long calendar gap still resolves the previous available data
   day without an unbounded returned history vector.
5. Changing the original CSV after snapshot creation leaves the selected
   historical close and signal unchanged. Snapshot history absent at run time
   stays absent even when the mutable source later gains older rows. Storage
   failure propagates rather than falling back to the source.
6. Engine and persisted-run integration show the first signal schedules its
   fill on the next actual open; warm-up does not affect bars processed,
   canonical record timestamps/counts, orders, fills, portfolio records, or
   Replay candle count. Replay remains read-only.
7. Existing prior-bar tests and literal v1 hashes remain unchanged. Task 17
   plans use v2 framing and change identity for each new functional field.
8. Qt tests and accessible help text disclose UTC data days, source-close
   extended-hours treatment, positive Down/Up magnitudes, inclusive threshold,
   and missing-history behavior without claiming an official exchange close.

## Files expected to change

- `Src/Backend/Data/Include/Bte/Data/ReleaseSnapshot.h`
- `Src/Backend/Data/Private/ReleaseSnapshot.cpp`
- `Src/Backend/Strategy/Include/Bte/Strategy/SelectableStrategy.h`
- `Src/Backend/Strategy/Private/SelectableStrategy.cpp`
- `Src/Backend/Engine/Include/Bte/Engine/Backtest.h`
- `Src/Backend/Engine/Private/Backtest.cpp`
- `Src/Bindings/Backtest/BacktestSessionVm.cpp` and its interface/tests
- `Src/Frontend/Backtest/BacktestTab.cpp` and its tests
- Strategy, Snapshot, Engine, Backtest binding, Replay determinism, and Qt unit
  tests for the behavior above
- `Docs/Specs/StrategyAuthoring.md`, `Docs/Specs/EngineReplayPnL.md`,
  `Docs/Specs/BacktestReplayProduct.md`, and `Docs/Plans/Issue10RemainingWork.md`
  to distinguish accepted behavior from implemented behavior

No strategy indicator, historical extrema, 30-day gain, Task 18 predicate,
Python Strategy path, or external market-data source is included.

## Maintainer review checklist

- [x] Approve the bounded one-row prior-data-day-close context at the
  immutable Snapshot/Bindings boundary.
- [x] Approve the Strategy state transition that uses the seed for the first
  executed date and rolls actual chronological bars for later data dates.
- [x] Approve v2 Strategy framing only when a new Task 17 source is present,
  preserving every legacy v1 hash.
- [x] Approve `DataSelectionIdentity.snapshotId` plus the UTC-date query rule
  as sufficient persisted source provenance; no warm-up rows enter Result or
  Replay.
- [x] Approve the UI mapping of positive magnitude plus Down/Up into signed
  inclusive comparisons.

The maintainer approved this written design on 2026-10-20. The implementation
plan is [`Task17UtcDataDayImplementationPlan.md`](Task17UtcDataDayImplementationPlan.md).
