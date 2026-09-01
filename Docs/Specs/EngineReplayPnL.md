# Engine, Replay, P&L, and Results

The project-owned C++ engine is the sole authority for chronology, orders,
fills, portfolio accounting, metrics, and durable results. Batch and Paced
Backtest execution use the same engine; pacing changes only when the next
`MarketSlice` is processed. K-line Replay is a separate presentation workflow
that reads a Backtest Result and never executes a Strategy or engine event.

## 1. Delivery status

| Status | Scope |
| --- | --- |
| **Implemented** | CSV-backed bar loading; forward/back bar stepping; replay clock controls; candlestick presentation; the compatibility starter Backtest slice; and a limited long-only Selectable Conditions slice. A selected buy/sell signal queues a whole-share market order for the next actual bar open, with 1 bp adverse slippage; buy cash failures preserve cash and open positions are marked at final close. |
| **Planned** | Volume presentation, synchronized multi-symbol `MarketSlice`, complete strategy interfaces and authoring, general order types, complete broker/portfolio/P&L behavior, short margin, corporate actions, metrics, immutable snapshots, canonical hashes, and SQLite `.bteresult`. |
| **Blocked** | Public release of the planned engine is blocked until pricing-data redistribution rights and a verified redistribution-cleared split manifest are documented. |

The current starter and selectable slices are intentionally single-symbol; the
selectable path supports only a flat position and whole-quantity long entry or
exit. Neither slice persists a canonical Backtest Result and neither is
evidence that the complete engine contract below is implemented. The starter
slice remains single-order. No durable result format is currently implemented;
the removed legacy JSON summary was not the target result contract.

### 1.1 Execution modes and scheduling

- A **Batch Backtest** processes slices without intentional presentation delay.
- A **Paced Backtest** lets Debug Run or a user-controlled execution surface
  advance the same engine one slice at a time. Pausing or stepping cannot alter
  functional inputs, event order, or canonical output.
- **K-line Replay** reads an existing completed or diagnostic Backtest Result
  and its referenced bars; it never calls Strategy hooks, starts Python, or
  creates new fills.
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
- Average-cost accounting applies to long and short positions. Partial close leaves remaining average cost unchanged; realized P&L uses the average cost and proportionally attributed entry/exit costs. It is not tax-lot accounting.
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

- Embed the exact Strategy source or typed plan used by the run. Export warns
  that embedded source is untrusted and must not be executed merely because a
  result was opened or imported.
- Write a same-filesystem staging file transactionally, validate and flush it,
  then atomically promote it on finalization.
- Crash recovery produces `Interrupted`, never `Completed`.
- `Failed`, `Canceled`, `Interrupted`, and `Incomplete` retain diagnostic events but expose no valid final performance metrics.
- The schema has explicit major and minor versions. The application reads the
  current and immediately previous major version. Unknown newer majors fail as
  `ResultSchemaUnsupported`; compatible minor additions are ignored only when
  their schema declaration permits it.
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

## 9. Determinism

The same engine version, immutable data/calendar/split snapshot, ordered universe, strategy artifact, runtime profile, seed, configuration, and numeric policy must produce identical canonical functional records and `canonicalResultHash`.

Canonical records include orders, fills, trades, portfolio/equity, costs,
strategy-relevant indicator snapshots, margin/corporate-action events, warnings,
and strategy logs in a specified stable order. Wall-clock creation time, local
paths, SQLite page layout, pacing delays, and UI state are excluded. CI compares
canonical bytes/hash, not physical SQLite file bytes. An intentional behavior
change requires a regression test, fixture update, and explanation.

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
