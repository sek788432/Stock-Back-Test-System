# Engine, Replay, P&L, and Results

The project-owned C++ engine is the sole authority for chronology, orders,
fills, portfolio accounting, metrics, and durable results. Batch and Paced
Backtest execution use the same engine; pacing changes only when the next
`MarketSlice` is processed. K-line Replay is a separate presentation workflow
that reads a Backtest Result and never executes a Strategy or engine event.

## 1. Delivery status

**Implemented lifecycle and R1–R3 durability:** Results retain recoverable
execution drafts outside the saved catalog and publish only through **Save
Result** or **Save & Replay**. Both publish the same recorded run idempotently;
only the latter navigates, and neither reruns Engine. Per-Result SQLite leases
coordinate supported processes. Immutable source references are pinned before
Staging/import visibility. The recorded Engine commits each completed bar's
canonical batch before advancing progress; a storage failure stops execution.
Recovery preserves legacy unleased staging with a warning and exposes only a
validated committed prefix as an **Interrupted Draft**. Automated process and
fault tests cover writer isolation, atomic persistence, recovery, and
preservation of legacy artifacts.

| Status | Scope |
| --- | --- |
| **Implemented** | CSV-backed bar loading; immutable snapshot/segment identity; forward/back bar stepping; replay clock controls; candlestick, volume, and persisted-fill presentation; transactional SQLite `.bteresult` storage; canonical hashing; the compatibility starter Backtest slice; and a limited long-only Selectable Conditions slice that records warnings, orders, fills, fixed-point slippage costs, and one post-slice portfolio checkpoint per processed bar. Task 17 and Task 18 UTC data-day conditions use bounded history from the same immutable snapshot without adding warm-up bars to Engine or Replay. |
| **Planned** | Synchronized multi-symbol `MarketSlice`, the complete Strategy/`MarketSlice` interface and authoring beyond the first-milestone Python workflow, general order types, complete broker/portfolio/P&L behavior, short margin, corporate actions, metrics, the complete Results-management UI and migration workflow, and the remaining canonical record families. |
| **Blocked** | Public product release of the planned engine is blocked until pricing-data redistribution rights, a verified redistribution-cleared split manifest, and exact snapshot hashes are documented. |

The current starter and selectable slices are intentionally single-symbol; the
selectable path supports only a flat position and whole-quantity long entry or
exit. They persist only the canonical families they can truthfully produce and
are not evidence that the complete engine contract below is implemented. The
starter slice remains single-order. The removed legacy JSON summary is not part
of the durable result contract.

Task 17's previous-data-day close condition is implemented in the limited
Selectable Conditions path and was merged in PR #81. It uses the previous
available UTC date's last source close from the same immutable snapshot,
including extended-hours rows, and excludes the current UTC date from the
reference. The one-row pre-range value seeds only Strategy state; Engine
records, processed-bar counts, and Replay candles remain within the selected
execution range. This CSV convention is not an official exchange-session close
and does not implement the general exchange-calendar contract below. Task 18's
30-data-day gain and prior-21-data-day extrema and breakouts are implemented in
the limited Selectable Conditions path. The binding requests the required pre-range UTC data-day summaries from the same
snapshot; Strategy uses them only as historical state and then rolls completed
dates as actual execution bars arrive. Insufficient history produces no
historical match. Warm-up contributes no processed bars, result records, or
Replay candles. Strategy v1 and v2 hash framing stay frozen; Task 18 sources use
v3 framing including the lookback period. See the
[`StrategyAuthoring.md`](StrategyAuthoring.md#csvutc-day-historical-conditions)
contract.

### 1.1 Execution modes and scheduling

- A **Batch Backtest** processes slices without intentional presentation delay.
- A **Paced Backtest** lets Debug Run or a user-controlled execution surface
  advance the same engine one slice at a time. Pausing or stepping cannot alter
  functional inputs, event order, or canonical output.
- The implemented `BacktestExecution` session is the shared stateful seam:
  Batch runs it to completion, while Paced callers invoke one `advance` per
  authorized slice. Terminal cancellation and operational failure retain only
  records from fully processed slices plus a terminal diagnostic.
- **K-line Replay** reads an existing completed or diagnostic Backtest Result
  and its referenced bars; it never calls Strategy hooks, starts Python, or
  creates new fills.
- Hourly and UTC Daily Replay use the exact saved selected spans, excluding
  bars before the first trusted portfolio checkpoint. Diagnostic results also
  exclude bars after the last processed checkpoint. Daily OHLCV uses only those
  trusted selected bars; broader source-day coverage is used solely to label
  Partial, never to add candles. Hourly and UTC Daily fills match the exact
  selected bar timestamps, including dates with interior missing-bar gaps. The
  saved records and canonical hash are unchanged by presentation.
- Result Replay loads and indexes the entire trusted candle timeline, while the
  presented chart starts with the first candle and exposes the untruncated
  `0..cursor` prefix. Playback, pause, step, speed, seek, and back advance or
  rewind the candle prefix and its persisted portfolio state together. A
  persisted fill is visible as a marker and synchronized trade row only when
  its containing candle is presented; rewinding hides future candles and fills.
  The prefix is not a sliding 500-frame window, and the terminal frame exposes
  the complete trusted timeline. Hourly and UTC Daily are alternate
  aggregations of the same saved time range.
- V1 permits one active Backtest. Advanced multi-run concurrency is outside the
  accepted scope.

## 2. Canonical event sequence

At each UTC timestamp the engine creates one synchronized `MarketSlice` containing only actual bars present for that timestamp. Symbol ordering is stable. It then performs:

1. Apply verified corporate actions effective before this slice.
2. Evaluate previously active orders against actual bars.
3. Apply fills to cash, positions, reservations, fees, and realized P&L.
4. Invoke `on_fill` for committed fills.
5. Update indicators with actual bars.
6. Mark positions, accrue applicable borrow cost, and evaluate margin.
7. Invoke one `on_market(ctx)` for the complete slice.
8. Validate and queue the callback's transactional commands.
9. Persist canonical events/checkpoints.

A strategy can never fill an order against the slice it used to decide that order. Missing bars remain absent; strategies and indicators never receive forward-filled bars. Portfolio marks may retain a stale last actual price, explicitly flagged with its timestamp.

V1 executes every hourly row in the immutable release snapshot, including pre-market and after-hours rows, under the named profile `vendorExtendedHours`. A versioned US-equities calendar in `America/New_York` defines exchange dates, regular/extended boundaries, holidays, early closes, and DAY expiry; event timestamps remain UTC.

## 3. Market data and corporate actions

- V1 reads an immutable, release-built snapshot derived from tracked `StockData/Extracted`; no runtime provider download, credential, user data import, or database integration exists.
- Release CI creates a manifest, hashes, `snapshotId`, content-addressed data segments, and calendar version. Runtime access is read-only.
- Results reference retained segments instead of duplicating OHLCV.
- Verified split events are applied before the first executable slice on the effective exchange date; if that symbol has no bar, before its next actual bar.
- Split metadata is never inferred from a price gap. Positions, average cost, active orders, reservations, margin, and relevant indicator history adjust together. Python receives `on_corporate_action` before `on_market`.
- Quantity remainders round toward zero to microshares and create a signed `SplitRoundingAdjustment` cash entry at the theoretical post-split reference price. Adjusted order prices never become more aggressive; zero-quantity orders cancel.
- Dividends are not modelled. Every result records `dividendAccounting: excluded` and warns that long returns may be understated and short returns overstated. Results are price-return, not total-return.

## 4. Orders and fills

V1 supports market, limit, stop, DAY, GTC, explicit OCO, and bracket orders. Stop-limit, IOC, FOK, partial fills, and liquidity simulation are unsupported and rejected explicitly.

### 4.1 Activation and price

- Market orders fill in full at the next actual bar open, with adverse slippage.
- Buy limit `L`: if `open <= L`, the unslipped candidate is the favorable open;
  otherwise if `low <= L`, it is `L`. Apply adverse slippage to that candidate
  but cap the final buy price at `L`. Sell limit is the exact mirror, with the
  final sell price never below its limit. Favorable opening gaps are therefore
  preserved and slippage never violates the limit.
- Buy stop `S`: if `open >= S`, fill at open; otherwise if `high >= S`, fill at `S`. Sell stop is the mirror. Stops retain adverse opening gaps and receive adverse slippage.
- A touched order fills its full quantity. The result discloses that V1 has no volume/liquidity or partial-fill model.
- DAY expires after the first complete eligible exchange session; GTC remains until fill, cancellation, terminal data, or run end.
- A pending order with no future market data cancels as `NoFutureMarketData`.

### 4.2 OCO and brackets

- An entry fill activates its protective stop and target for the next actual bar.
- The first filled exit cancels its sibling.
- If both exits are touched within one OHLC bar and the open does not establish an order, the adverse exit fills first. Persist `IntrabarAmbiguityResolved` and explain this conservative hourly-bar assumption in user documentation.

### 4.3 Cancellation, replacement, and priority

- A callback cancellation cannot undo a fill already applied in that slice.
- Filled, canceled, expired, or unknown orders return `OrderNotCancelable`.
- Replacement atomically cancels the immutable old order and creates a new ID active on the next actual bar. Results link both IDs; replacing one OCO member preserves the group.
- Same-slice order priority is: engine forced liquidation; protective OCO/bracket exits; other exposure reductions; exposure increases. Ties use activation timestamp, then ascending order ID.
- Cash, reservations, positions, and margin are recalculated after every fill.

## 5. Portfolio, buying power, and costs

- There is one net position per symbol. An order may reduce or close the opposite position but may not cross zero; reversal requires an explicit close and a later opposite entry.
- Long purchases use unrestricted cash only. V1 has no long leverage, debit balance, or margin interest.
- Long-sale proceeds are reusable immediately under `instantSettlementV1`.
- Average-cost accounting applies to long and short positions. Partial close leaves remaining average cost unchanged; realized P&L uses the average cost and proportionally attributed entry/exit costs. The allocated open basis is rounded to the nearest microdollar using half-even; unallocated microdollars remain in the open basis, and a full close consumes that exact remainder. It is not tax-lot accounting.
- Exposure-increasing orders reserve estimated buying power. Buy limits reserve the limit-price cost; market/stops use latest price plus costs; shorts also reserve initial margin.
- If the actual gap price makes a full fill unaffordable, reject it as `InsufficientBuyingPowerAtExecution`, release the reservation, and never resize or create negative unrestricted cash.
- Default `retailUsEquityV1` is zero commission and 1 basis point adverse slippage. Per-run overrides are allowed and persisted. Forced covers use market slippage; limits still cannot violate their price.

## 6. Short selling and margin

- Borrow availability is `alwaysAvailable`; V1 does not simulate locates, hard-to-borrow lists, or recalls.
- Borrow fee is fixed and non-configurable at 3% annually on a 360-day basis, accrued by calendar day and rounded conservatively.
- Under `regTStaticV1`, short-sale proceeds are restricted and cannot fund other trades. Opening a short requires additional unrestricted cash equal to 50% of short market value (150% collateral including proceeds).
- Maintenance for a stock priced at least $5 is the greater of 30% of current short market value or $5/share. Below $5 it is the greater of 100% or $2.50/share.
- Margin is checked after every slice mark. A breach enters `MarginDeficient`, cancels pending strategy orders, rejects new strategy commands as `MarginLiquidationActive`, and gives the engine exclusive order control.
- No grace period or simulated deposit exists. Forced covers queue for next actual opens and continue only until the 50% initial-margin level is restored.
- Cover the position with the largest maintenance requirement first; ties use ascending symbol. Re-rank after each fill. A partial cover may restore compliance.
- Symbols without a current executable bar stay queued while available positions may be covered. No stale or synthetic fill is permitted.
- If no future bar can complete required liquidation, preserve the open short and save `Incomplete: UnliquidatableMarginDeficit`; final performance metrics are invalid.
- Normal strategy commands resume on the first slice after margin is restored.

## 7. Numeric and rounding policy

Authoritative accounting uses checked fixed-point strong types:

| Type | Storage and scale |
| --- | --- |
| `Price` | signed int64 nanodollars (`10^-9` dollar) |
| `Quantity` | signed int64 microshares (`10^-6` share) |
| `Money` | signed int64 microdollars (`10^-6` dollar) |
| `Rate` | signed int64 parts per billion |

- Overflow and invalid precision are errors.
- General monetary conversion rounds half-even once at the domain seam; intermediate arithmetic retains checked higher precision.
- A partial close allocates `openCostBasis × closedShares / openShares` to the closed shares, rounded once to the nearest microdollar using half-even. Compute the quotient and remainder without overflowing an intermediate product, subtract the allocated integer basis from the remaining basis, and use the exact remaining basis on a full close.
- Half-even tie parity applies to the complete integer monetary amount, not
  just a fractional-product component. For example, one share marked at
  $1.0000015 becomes 1,000,002 microdollars; three shares become 3,000,004.
  Computing parity must not overflow before the checked final addition.
- Slippage-adjusted buys round price up and sells down.
- Commission, borrow fee, and margin requirements round upward.
- Forced-cover quantity rounds upward only as far as required to restore margin.
- Canonical persistence uses integer units or decimal strings, never JSON floating-point values.
- The numeric and rounding policy version is stored in every result.

## 8. Run completion and durable results

- Open end-of-run positions remain open and use their last actual mark; no synthetic terminal liquidation occurs. Realized and unrealized P&L remain separate.
- Pending orders cancel as `EndOfRun`; borrow accrual stops at the configured run-end timestamp.
- A final mark is valid only if the symbol has an actual bar in the final exchange session. Otherwise save `Incomplete: StaleFinalMark`, retain the diagnostic mark and age, and suppress final metrics.

The schema, lifecycle, framing, compatibility, and recovery rules are fixed by
the [canonical result storage decision](../Decisions/ImportantDecisions.md#canonical-result-storage-and-lifecycle).
Each run targets one transactional SQLite
`.bteresult` file containing typed
run/configuration, strategy source, orders, fills, trades, positions, equity,
fees, margin, corporate actions, strategy-relevant indicator snapshots,
warnings, logs, and data-segment references.

Schema 2 also persists one validated capability-declaration row. The current
limited Engine declares warnings, orders, fills, post-slice portfolio,
slippage-cost, and terminal-diagnostic records as supported. Replay derives
realized P&L from persisted long-only fill amounts using average cost and derives
unrealized P&L as total recorded P&L less realized P&L. A valid positive-price
fill may round to zero microdollars and remains replayable. Partial-close basis
allocation follows the half-even rule in §7. These presentation values
cover the current full-fill path; they do not add persisted P&L record families,
short positions, partial fills, or fee models. Replay also exposes the zero-based
selected source-bar index for each fill marker. Trades, metrics,
strategy logs, indicator snapshots, corporate actions, and margin records are
explicitly unsupported; readers expose these flags and the limited Engine never
fabricates those families. Because this declaration is invariant for schema 2
rather than a per-run functional value, it is container schema metadata and is
not an additional canonical-hash frame.

- Embed the exact Strategy source or typed plan used by the run. Export warns
  that embedded source is untrusted and must not be executed merely because a
  result was opened or imported.
- Write a same-filesystem staging file transactionally, validate and flush it,
  then atomically promote it on finalization.
- Crash recovery produces `Interrupted`, never `Completed`.
- `Failed`, `Canceled`, `Interrupted`, and `Incomplete` retain diagnostic events but expose no valid final performance metrics.
- The schema has explicit major and minor versions. Schema 2 is the first
  released canonical container. The implemented reader accepts schemas 2 and 3
  under separate validators and hash framings; older prototypes and unknown
  newer majors fail as `ResultSchemaUnsupported`. Schema-2 bytes are never
  reinterpreted as schema 3. A later major must read and
  validate the immediately previous released major and create a new copy.
  Compatible minor additions may be ignored only when their schema
  declaration permits it.
- Migration validates the source and creates a new copy; it never rewrites the
  original.
- Imports are untrusted data. They validate size, schema, canonical hash,
  identifiers, and data references without executing embedded Strategy source.
  Missing data reports `DataSnapshotUnavailable` rather than substituting data.
- User deletion moves a result to 30-day Trash. Active and trashed results pin
  Data Segments. Results never pin Runtime Profiles because replay does not
  execute source. Purge releases references; unreferenced segments follow the
  separate Data Trash lifecycle in [`DataLayer.md`](DataLayer.md).
- K-line Replay reads persisted engine events, indicator snapshots, and
  referenced immutable bars; it does not rerun Python, Strategy hooks, or the
  C++ execution engine.

### 8.1 Implemented schema 2 canonical hash framing

The current limited Results implementation uses result schema integer `2`,
numeric-policy literal `fixed-point-v1`, and aggregation-policy literal
`utcCalendarDayV1`. Its `canonicalResultHash` is lowercase hexadecimal SHA-256
over the exact byte stream below. This subsection is normative for schema 2 and
matches the implemented writer and reader; it does not describe the still-
planned complete record model.

The stream begins with the 20 ASCII bytes `BTE-CANONICAL-RESULT` followed by one
zero byte. Every following field is one frame:

1. one ASCII tag byte;
2. an unsigned 64-bit big-endian payload byte count;
3. exactly that many payload bytes.

An integer payload is always eight bytes containing the signed 64-bit value in
two's-complement big-endian form. Non-negative ordinals, row counts, sequences,
and enum values use that same signed representation. A string payload is the
exact stored byte sequence with no terminator or transcoding. For an optional
integer, absence is a zero-length frame and presence is the normal eight-byte
integer frame; therefore an absent value is distinct from present zero.

Frames occur in this exact order:

| Scope | Repetition and frames |
| --- | --- |
| Header | `v` schema integer (`2`) |
| Ordered universe | one `u` string for each symbol, preserving Run Configuration order |
| Run Configuration | `s` range-start Unix milliseconds, `e` range-end Unix milliseconds, `c` initial-capital microdollars, `i` strategy ID, `h` strategy hash, `n` numeric-policy literal, `a` aggregation-policy literal |
| Data selection | five `d` strings: snapshot ID, calendar hash, split-manifest hash, source timeframe, and profile, in that order |
| Each ordered data span | `o` snapshot-manifest ordinal, `y` symbol, two `g` strings for segment ID then segment hash, two `r` integers for first row then row count, and two `t` integers for first then last Unix-millisecond timestamp |
| Each canonical record | `q` sequence, `t` Unix-millisecond timestamp, `y` symbol, `f` record-family enum, `x` order-side enum, optional integers `1` quantity shares, `2` price nanodollars, `3` amount microdollars, `4` cash microdollars, `5` market-value microdollars, `6` equity microdollars, `p` P&L microdollars, `7` position shares, then `m` text |
| Terminal state | `z` run-status enum, `m` terminal-reason text, optional `8` final-equity microdollars, optional `9` P&L microdollars |

Schema-2 enum integers are fixed: run status is `running=0`, `completed=1`,
`failed=2`, `canceled=3`, `interrupted=4`, and `incomplete=5`; record family is
`order=0`, `fill=1`, `portfolio=2`, `cost=3`, `warning=4`, `log=5`, and
`terminalDiagnostic=6`; order side is `none=0`, `buy=1`, and `sell=2`.

The current C++ Strategy hash is derived by the Strategy module, never supplied
by the UI: the built-in starter uses a versioned identity and Selectable
Conditions hash the ordered buy/sell groups plus every condition and indicator
field using their exact enum, integer, and IEEE-754 bit values.

Universe order is functional. Data spans are hashed in vector order and must
have strictly increasing snapshot-manifest ordinals; a selection may begin at
any manifest segment. Canonical records are hashed in
sequence order; persisted sequences must be exactly `0..N-1`, and timestamps
must be nondecreasing. Stable same-timestamp ordering is therefore the assigned
sequence, not database row order or enum family.

The schema-2 hash includes every field named above. It excludes Result ID,
creation/save wall-clock times, catalog availability and ordering, local paths,
SQLite tables/pages/journals/transaction metadata, pacing delays, and UI state.
OHLCV bytes are not copied into the result hash stream; their snapshot,
segment, span, calendar, and split identities are included instead. Schema 2
does not yet represent the target engine-version, strategy-API/source,
cost-profile, Runtime Profile, complete numeric-policy, or general record-family
contracts. Those remain required before general release rather than being
silently treated as stable omissions.

Any change to the domain separator, tag, frame encoding, field inclusion,
enum integer, or ordering rule requires a new result schema version plus an
explained deterministic-fixture update. A reader must never reinterpret schema
1 bytes under changed rules.

### 8.2 Implemented schema 3 provenance and schema-2 copy migration

Schema-3 provenance, writes, reads, canonical hash framing, and bounded Python
diagnostic persistence are implemented and covered by registered round-trip,
tampering, compatibility, and literal-hash tests. The internal
`ResultStore::migrateSchema2Copy` operation creates and validates a new
schema-3 copy from schema 2; no user-facing migration action invokes it yet.
Schema 2 and its §8.1 framing remain frozen. A schema-3 reader dispatches by
container version before parsing version-specific records and never retries a
failed schema-3 parse as schema 2 or vice versa.

Every schema-3 Result stores and validates:

- Strategy mode, API version, template version, and canonical JSON parameters;
- for Python mode, exact normalized UTF-8 source bytes and SHA-256 source
  hash; for Selectable Conditions, the exact typed plan and its existing plan
  hash; legacy copies explicitly mark unavailable strategy artifacts as
  `legacy-unrecorded` instead of fabricating them;
- Engine build/version and complete run configuration, including timeframe,
  costs, numeric policy, and seed;
- Runtime Profile ID and canonical manifest hash, CPython version/ABI, and
  exact package names, versions, wheel hashes, and project-owned `stockbt`
  source hash;
- ordered universe, immutable snapshot/calendar/split identities, and exact
  ordered data spans;
- canonical event records and terminal status/reason; and
- a `provenanceAvailability` entry for each versioned provenance field, with
  `recorded` or `legacy-unrecorded` state, so missing legacy facts are never
  represented as facts.

The schema-3 `canonicalResultHash` is lowercase hexadecimal SHA-256 over a
stream beginning with domain separator ASCII `BTE-CANONICAL-RESULT-V3`
followed by one zero byte. Every following frame is one ASCII tag byte, an
unsigned 64-bit big-endian payload byte count, and that many UTF-8 bytes of RFC
8785 canonical JSON. Frames stream in this exact order:

| Frame | Repetition and content |
| --- | --- |
| `h` | One header object with schema version `3`, hash-frame version `3`, Engine build, numeric and aggregation policies, complete Run Configuration, strategy mode/ID/hash, provenance-availability map, and ordered symbols |
| `s` | One strategy object with the exact Python source bytes and source SHA-256 or exact typed Selectable Conditions plan and plan hash, plus API/template versions and canonical parameter object; a legacy-unrecorded artifact is represented explicitly as unavailable |
| `r` | One runtime object with Runtime Profile ID/manifest hash, CPython ABI/version, exact package names/versions/artifact hashes, and `stockbt` source hash; absent only for non-Python modes and represented as explicit null |
| `d` | One immutable data-selection object with snapshot, calendar, split manifest, source timeframe, and aggregation/profile identities |
| `p` | One frame per ordered data span, preserving manifest ordinal and exact segment ID/hash, rows, and timestamps |
| `e` | One frame per canonical event in Engine sequence order, with schema-defined event family, symbol, timestamp, integer-valued domain fields, and bounded diagnostic/log payloads |
| `t` | One terminal object with run status, terminal reason, optional final fixed-point values, and diagnostic completeness state |

The schema-3 JSON frame shapes are fixed. Object keys are exactly those shown
below (with optional values represented by explicit JSON `null`); JCS provides
the required UTF-16 key ordering. The `h` object has `aggregationPolicy`,
`commissionRatePartsPerBillion`, `costProfile`, `engineBuild`,
`hashFrameVersion`, `initialCapitalMicrodollars`, `numericPolicy`,
`provenanceAvailability`, `rangeEndUnixMillis`, `rangeStartUnixMillis`,
`schemaVersion`, `seed`, `slippageRatePartsPerBillion`, `strategyHash`,
`strategyId`, `strategyMode`, `timeframe`, and `universe`. Schema and frame
versions are JSON integers; every other integer value, including ordinals and
event sequences, is a canonical base-10 JSON string. Availability keys are
`aggregationPolicy`, `costs`, `dataSelection`, `engineBuild`, `numericPolicy`,
`runtimeProfile`, `runtimePackages`, `seed`, `strategyApi`,
`strategyArtifact`, `strategyMode`, `strategyTemplate`, and `timeframe`; each
value is `recorded` or `legacy-unrecorded`.

The `s` object has `apiVersion`, `canonicalParameters`, `canonicalPlan`,
`planHash`, `sourceHash`, `sourceUtf8`, and `templateVersion`. Parameters and a
typed plan are embedded JSON values, not escaped JSON text. Exactly one of
`sourceUtf8`/`sourceHash` and `canonicalPlan`/`planHash` is populated for a
recorded Python or Selectable Conditions artifact; all four are `null` for an
unavailable legacy artifact. `canonicalParameters` is an object for recorded
artifacts and `null` when legacy-unrecorded. The `r` object has `abi`,
`manifest`, `manifestHash`, `packages`, `profileId`, `pythonVersion`, and
`stockbtSourceHash`; each package object has `artifactHash`, `name`, and
`version`. `manifest` is the parsed canonical manifest object. A known
non-Python mode uses `null` for the whole `r` payload. A schema-2 migration
uses `null` only when its strategy mode is known to be non-Python; otherwise
the availability map identifies missing runtime provenance.

The `d` object has `aggregationProfile`, `calendarHash`, `snapshotId`,
`sourceTimeframe`, and `splitManifestHash`. Each `p` object has `firstRow`,
`firstTimestampUnixMillis`, `lastTimestampUnixMillis`, `manifestOrdinal`,
`rowCount`, `segmentHash`, `segmentId`, and `symbol`.
Each `e` object has `amountMicrodollars`, `cashMicrodollars`,
`diagnosticPayload`, `equityMicrodollars`, `family`, `marketValueMicrodollars`,
`pnlMicrodollars`, `positionShares`, `priceNanodollars`, `quantityShares`,
`sequence`, `side`, `symbol`, and `timestampUnixMillis`; all optional numeric
fields are strings or `null`, and `family`/`side` use fixed lowercase names.
The `t` object has `diagnosticComplete`, `finalEquityMicrodollars`, `pnlMicrodollars`,
`status`, and `terminalReason`.
`diagnosticComplete` is true only when all diagnostics produced by the run were
captured within their declared bounds and durably persisted. Truncation or
recovery that cannot establish a complete diagnostic stream sets it false; it
is persisted independently from terminal status and participates in the hash.

Every `int64` domain value is a canonical base-10 string; monetary,
price, and quantity values use their declared integer fixed-point units; enum
fields use fixed schema-defined strings; JSON floating-point values use RFC
8785 serialization. Source bytes are embedded as UTF-8 and their byte hash is
included. Runtime/package manifests are embedded as canonical bytes and their
hashes are included. Result ID, migration ID, wall-clock create/save times,
local paths, SQLite layout, pacing/UI state, and reproducibility evidence are
excluded. `provenanceAvailability` and explicit null values distinguish an
absent legacy value from an ordinary omitted field. A new required functional
identity field, encoding, enum, or ordering rule requires another Result
schema major. Streaming frames keeps hash calculation bounded; it never builds
one in-memory JSON document from the full event history.

Python exception, timeout, cancellation, protocol/runtime failure, and bounded
structured logs are persisted as Result diagnostics with stable error code,
hook/phase, source line/column when available, captured timestamp, traceback,
and truncation counts. These are included in the canonical event stream when
they are Result events; the reproducibility-check coordinator record described
below is not a Result event and cannot change a Result hash. The Result's
terminal status remains `Completed`, `Failed`, or another execution status.
`reproducibilityStatus` is separate non-hash metadata and is `unverified`
unless valid matching evidence exists for its exact functional-input identity.

The backend schema-2-to-3 operation validates the complete source schema-2
container, its schema-2 canonical hash, identifiers, records, and every
referenced immutable data segment before copying. It writes a new Result ID
and a `migrated-from` identity that
contains the old Result ID and old schema-2 hash. It preserves event order,
display metadata, data references, status, and capability declarations. It
marks each schema-3 provenance field absent from schema 2 as
`legacy-unrecorded` in the availability map; it never invents Python
source/runtime/seed evidence or executes embedded source. It computes a new
schema-3 hash under the schema-3 framing, and never claims that hash equals the
source's schema-2 hash.

Migration writes to a same-store staging destination, checkpoints and closes
it, opens and validates the completed schema-3 copy and all segment references,
then promotes it without clobbering an existing Result. A destination ID
collision, unsupported/corrupt schema-2 source, invalid reference, source or
destination hash mismatch, or any copy, write, validation, or catalog-visibility
failure removes the new copy and releases its data references; the original
remains byte-for-byte unchanged and visible. Registered tests independently
verify source schema-2 and copy schema-3 hashes and cover source corruption,
unavailable data, destination collisions, failure cleanup, and reopening both
copies. Migration never publishes an invalid copy and never runs Python.
Schema 2 stays independently openable after migration; there is no automatic
or in-place migration. The **Create Schema-3 Copy** UI action remains planned.

### 8.3 Reproducibility evidence

**Implementation status:** the durable evidence store and sequential
three-attempt coordinator are implemented and tested. The Qt Verify workflow
uses this coordinator. End-to-end acceptance with the pinned production Python
runtime remains blocked until that runtime is assembled and import-tested.

The Verify Reproducibility action freezes one functional-input
identity containing exact
source hash, canonical parameters, API/template version, Engine build, runtime
profile ID/hash, seed, numeric and cost policies, ordered symbols, immutable
snapshot/data spans, timeframe, range, and remaining run configuration. It
launches at most one Backtest at a time and starts three fresh workers
sequentially from those same frozen inputs. Each attempt is a normal
unverified Result draft and is finalized through the existing explicit
Save/Discard policy; verification does not auto-publish, change its canonical
records/hash, or alter a finalized Result.

A separate application metadata artifact at
`Results/Verifications/<verification-id>.bteverify` contains a versioned RFC
8785 JSON record of at most 64 KiB, an integrity hash, the verification ID,
frozen functional-input identity, state, progress ordinal, and up to three
attempt entries. The app atomically replaces and flushes that artifact after
each attempt-state transition; it is not a Result and is not published to the
Results catalog. A corrupt record is preserved for diagnosis and cannot
verify a Result. Each attempt entry records its exact Result ID when a writer
was created, terminal status, canonical Result hash when available, and
structured startup/runtime error otherwise. Its terminal states are
`verified`, `mismatch`, `failed`, `canceled`, and `interrupted`; partial
attempts are retained as unverified evidence. Only three completed attempts
with identical canonical functional hashes and identical frozen input
identities produce `verified`. The run's terminal status remains `Completed`;
`reproducibilityStatus` is separate metadata and is `unverified` until valid
matching evidence exists. The first mismatch, failure, cancellation, or
interruption stops further launch. Recovery of any nonterminal verification
sets it to `interrupted`; it never resumes automatically or receives a
verified label. Evidence may label a Result only when its complete frozen
identity matches that Result. Discarding an attempt draft removes that
attempt's detailed records under the existing explicit policy but does not
rewrite the verification record or promote an incomplete set to verified.

Opening, validating, migrating, saving, and replaying a Result never starts
Python. Replay remains available when the profile is missing or tampered.

## 9. Determinism

Result Replay uses `ResultStore::openResultWithData()` to validate the stored
result and read its exact immutable selection once, returning owned records
and bars together. `openResult()` retains its existing validation contract.
Daily Replay separately reads full UTC-day source coverage for Partial labels;
this wider read never adds candles to the saved selection.

The same engine version, immutable data/calendar/split snapshot, ordered universe, strategy artifact, runtime profile, seed, configuration, and numeric policy must produce identical canonical functional records and `canonicalResultHash`.

Canonical records include orders, fills, trades, portfolio/equity, costs,
strategy-relevant indicator snapshots, margin/corporate-action events, warnings,
and strategy logs in a specified stable order. Wall-clock creation time, local
paths, SQLite page layout, pacing delays, and UI state are excluded. CI compares
canonical bytes/hash, not physical SQLite file bytes. An intentional behavior
change requires a regression test, fixture update, and explanation.

The registered Qt-enabled `ReplayBacktestDeterminism` suite includes the
offline `Tests/Fixtures/ReplayMultiFill/SYN.csv` fixture. It selects 12 hourly
rows from immutable three-row segments, executes real Selectable Conditions
through Engine, and checks five buys/five sells at the next actual opens.
Independent literal fill/cost/portfolio ledgers are compared with persisted
records; candle/fill/portfolio values are checked in Hourly/UTC Daily replay.
Repeated runs must have distinct Result IDs but equal records and hashes.
Exclusive-end, excluded-next-open, invalid
plan and zero-signal cases are also covered. This synthetic test symbol is not
application market data. These checks do not establish the separate literal
canonical-hash oracle, crash-durability, Daily interior-gap, or complete
engine/replay acceptance requirements.

## 10. Trades and metrics

Completed runs report total return, realized/unrealized P&L, commissions, slippage, borrow cost, exposure, maximum drawdown and duration, trade count, win rate, average win/loss, payoff ratio, expectancy, CAGR, volatility, Sharpe, and Sortino.

- A **Trade** is one flat-to-flat position episode for one symbol. It starts when
  net quantity leaves zero and ends when it returns to zero. Scale-ins and
  partial closes belong to that same Trade; an open episode is not a completed
  Trade for win-rate calculations.
- Run validation requires strictly positive initial capital and a finite annual
  risk-free rate greater than `-1`. Invalid values fail the Run Configuration
  before any metric or engine event. Total return is
  `(finalEquity / initialEquity) - 1`.
- CAGR is `(finalEquity / initialEquity)^(365.2425 / elapsedDays) - 1` for a
  positive elapsed interval and positive endpoint equities; otherwise it is
  unavailable with a structured reason.
- Session returns use consecutive exchange-session-end equity values. Annualized
  volatility is their sample standard deviation times `sqrt(252)` and requires
  at least two session returns; otherwise it is unavailable.
- The persisted annual risk-free rate becomes the effective session rate
  `(1 + annualRate)^(1 / 252) - 1`; session excess return is session return
  minus that rate. Sharpe is the mean session excess return divided by its sample
  standard deviation times `sqrt(252)` and requires at least two session
  returns. Sortino uses the same numerator and
  `sqrt(sum(min(excessReturn, 0)^2) / sessionReturnCount)` as its downside
  denominator; it requires at least one session return and at least one strictly
  negative excess return. Insufficient samples or zero denominators make the
  ratio unavailable rather than infinite.
- Maximum drawdown is the greatest `1 - equity / priorPeakEquity` over ordered
  session-end equity. Duration counts exchange sessions from the peak through
  recovery, or through the end when unrecovered.
- Win rate is profitable completed Trades divided by completed Trades. Payoff
  ratio is mean winning net P&L divided by the absolute mean losing net P&L.
  Average win and average loss are the arithmetic means of positive and
  negative net completed-Trade P&L respectively. Expectancy is mean net P&L
  across completed Trades, and trade count is the number of completed Trades.
  Empty required sets and zero denominators are unavailable with structured
  reasons.
- Realized P&L, unrealized P&L, commission, slippage, and borrow cost are direct
  sums of their canonical ledger records in `Money`. Slippage cost is the signed
  adverse difference between the unslipped executable candidate and actual
  fill, multiplied by fill quantity and normalized through the numeric policy.
- Exposure is the time-weighted mean of `abs(netMarketValue) / equity` across
  consecutive canonical slice intervals; intervals with nonpositive equity make
  the metric unavailable. The final sample has zero duration and adds no weight.
- Risk-free rate defaults to zero and is persisted per run.
- Benchmark alpha/beta is **Planned**, not V1.
- Non-completed runs expose labelled diagnostic values only.

## 11. Required verification

- Every public engine, order, fill, portfolio, margin, result, replay, and metric behavior requires positive, negative, and boundary unit tests.
- Deterministic fixtures cover event order, gaps, missing bars, OCO ambiguity, buying power, short margin, corporate actions, rounding, result recovery/import, and backtest/replay parity.
- Batch and Paced Backtest execution must yield the same canonical functional
  records for identical inputs.
- K-line Replay tests must prove result presentation does not invoke Strategy
  hooks, Python, indicator scheduling, order evaluation, or fill creation.
- Every bug fix or intentional behavior change requires a regression test that would fail against the old behavior.
- Performance tests cover the per-slice hot path without weakening correctness tests.
- A check is merge-blocking only when [`CiDevFlow.md`](CiDevFlow.md) marks its implemented gate as required.
