# Replay chart viewport and presentation design

**Status:** Implemented and merged in the Issue 10 delivery. The ordered-bar
sliding viewport, chart-first Replay layout, linked price and volume
coordinates, and Follow latest / Fit reached controls passed the native
multi-size workflow and automated long-range reconciliation. Geometry tests
assert the eight-logical-pixel trade-marker clearance. Direct `Launch.sh`
startup remains unavailable in the shell's display-restricted environment.
Native 31,466-bar Hourly `max` playback reached its final frame; the initial
automation request was delayed, so no throughput benchmark is claimed. See the
[Issue 10 acceptance ledger](../Plans/Issue10RemainingWork.md). The owning
product and Qt specifications remain
[`BacktestReplayProduct.md`](BacktestReplayProduct.md) and
[`FrontendQt.md`](FrontendQt.md); the delivery ledger is
[`Issue10DeterministicReplayFixturePlan.md`](../Issue10DeterministicReplayFixturePlan.md).

## Intent and scope

The maintainer wants Replay to read as one attractive, conventional K-line
chart: one selected Hourly or UTC Daily bar per candle, price above a compact
volume histogram, a shared time axis, and Buy/Sell markers beside their exact
containing candles. Playing a long result must not make all candles smaller and
smaller. Wheel/button zoom and horizontal review must work without losing bars,
altering result data, or exposing future bars. The existing chart-first Replay
layout, persisted result identity, and progressive playback remain in scope;
market-data selection, execution, storage formats, and backtest semantics do not.

## Approaches considered

1. **Recommended: one visually continuous chart surface with a sliding bar
   viewport.** Allocate a large upper price region and a compact lower volume
   region with one shared horizontal bar coordinate and one bottom UTC time
   axis. Keep the complete reached prefix in the presentation model while the
   viewport shows a readable subset. This separates replay progress from
   navigation and scales to long results.
2. **Two framed panes with linked axes and a rolling viewport.** This requires
   less change to the current Qt Charts adapter but retains the disconnected
   two-screen appearance the maintainer rejected.
3. **Fit the entire reached prefix after every frame.** This is close to the
   current implementation and keeps every reached candle in the viewport, but
   23,777 bars cannot each occupy meaningful pixels in a desktop-width chart.
   It is rejected as the default playback behavior.

This is a presentation design, not a decision to use TradingView code, assets,
branding, or a particular chart library. The first-party interaction research
is recorded in [`TradingViewChartInteraction.md`](../Research/TradingViewChartInteraction.md).

## Chart surface and coordinate contract

- Present price and volume in one continuous chart frame, without two large
  independently framed screens. Start with approximately 80–85% of the plot
  height for price and 15–20% for volume; tune the final ratio by inspecting
  1024×768, 1200×800, and 1440×900 actual app windows. A subtle separator is
  acceptable, but volume must not have a second independent time-label row.
- Price and volume share an x coordinate for each selected bar. Each bar has
  one OHLC candle above and one direction-colored volume column below. UTC
  Hourly labels include date and hour; UTC Daily labels include date. Price
  uses quote-price ticks and volume uses whole-share ticks on separate vertical
  scales. Volume values and Buy/Sell glyph geometry never enter price autoscale.
  The Qt development adapter aligns automatic plot layouts through shared
  left insets, not manual plot rectangles, so rendered time grids agree with
  candles and volume columns. Layout-driven viewport updates are queued to
  avoid axis-update reentry. Retain the widest intrinsic label requirement,
  excluding previously compensated margins so integer rounding cannot accumulate
  blank space during progressive playback, within a timeline
  to avoid edge-bar/label feedback; reset it on replacement or timeframe change.
- The horizontal chart uses ordered bar slots with a stable center for each
  selected bar. Labels report the underlying UTC timestamps; weekends, closed
  hours, and missing source bars do not invent candles. This keeps adjacent
  observed candles visually distinguishable even when elapsed-time gaps vary.
  The actual timestamp remains available in the crosshair/tooltip and trade
  log. A missing-hour gap may be indicated, but may not silently synthesize a
  bar or move a persisted fill to another bar.
- Price auto-range is computed only from the high/low of candles currently in
  the viewport, with a bounded visual margin. Volume auto-range uses only
  visible volume and includes zero. Flat-price, high-volume, sparse-prefix,
  and zero-volume cases retain readable scales. Markers may use reserved
  visual margin outside the candle body but cannot rescale the price data.

## Replay and viewport state

The selected immutable timeline and replay cursor are independent of the
viewport's first/last visible bar, density, and follow mode. The chart may
render only the reached prefix, but it must retain access to every reached
bar; it must never cap that prefix at 500 or mutate source OHLCV/fills.

- Opening a result shows exactly its first selected candle at a normal candle
  width, with sensible surrounding whitespace. Playback, Step, seek forward,
  and speed changes reveal bars strictly through the current replay cursor.
  Back/seek backward remove future candles, volume, markers, and trade rows
  from presentation without erasing the immutable timeline.
- **Follow latest** is the default viewport mode. Once enough bars have
  appeared to fill the chosen density, the viewport slides forward with the
  newest candle near the right side. Prior reached bars remain accessible by
  horizontal pan. New frames do not widen the visible range to include the
  entire reached prefix. A normal playback candle should remain visually
  distinct at all tested sizes; the revised default target is roughly
  22–26 px per bar slot and 16–20 rendered pixels per candle body, with the
  body narrower than its slot. This supersedes the initial 12–16 px slot
  target after actual desktop use found it too thin.
- Wheel zoom and In/Out controls adjust horizontal bar density, bounded so
  normal candle rendering never reaches zero width or an empty viewport.
  Wheel zoom is anchored to the candle under/nearest the pointer; button zoom
  anchors to the viewport center. Zoom does not change replay cursor or saved
  result identity. Zoom while following keeps the chosen density and continues
  following subsequent frames. If pointer-anchored zoom temporarily moves the
  newest reached candle out of view, **Follow latest** remains available and
  recenters immediately; it must not be disabled solely because the next
  playback frame would auto-follow.
- Horizontal drag/pan enters **Review history** mode. Playback may continue,
  but it does not yank the user away from the inspected bars. A visible
  **Follow latest** action returns to the newest reached candle without
  changing the cursor. The playback control state must make clear that the
  replay can advance while the user is reviewing older bars.
- Reset restores the default density and Follow latest. An explicit **Fit
  reached bars** overview may be offered only when every reached bar can be
  drawn at a minimum legible slot width. For larger prefixes it is disabled
  with an explanatory reason; it must not silently compress thousands of
  candles into subpixel bodies. All reached bars are still accessible by pan.
- Switching Hourly/Daily rebuilds the displayed series from the saved result
  and maintains the existing UTC-date cursor mapping. It resets viewport
  density/follow in a documented, deterministic way. Resize preserves the
  visible anchor and selected density as far as the new width permits, then
  clamps to reached bars. Neither action may produce an empty chart when a
  reached bar exists.
- Buy ▲ and Sell ▼ remain anchored to the containing candle's x slot and
  below its low/above its high, with eight logical pixels between the glyph
  bounding box and the wick, independent of horizontal zoom. The triangle tip
  has slightly greater clearance. Reserve a fixed 25-pixel visual margin per
  price edge at supported Replay sizes. Cap the pixel-derived padding at half
  the visible price span for short plots; retain the one-USD base padding floor
  for flat/near-flat prices. Plots under 100 logical pixels may not fit the full
  reserve. This visual padding operates without
  using fill prices or volume in autoscale. Recompute on height-only resize.
  They appear only after their persisted fill
  bar is reached, stay aligned through zoom/pan/resize/timeframe changes, and
  never reveal future fills. Multiple reached Buys in the same displayed candle
  share one Buy triangle with no leader; the tooltip retains the first fill's
  details and states the reached Buy count, with every fill available in the
  trade log. This is grouped by candle rather than identical fill timestamps.
  Single Sells have no leader; coincident Sells retain their deterministic
  positioning leaders. Replacement/rewind/timeframe rebuilds reset Buy groups
  and counts. Exact per-fill financial points and trade rows remain intact.

## Component boundary and errors

`ResultReplay` continues to own the validated immutable timeline and replay
cursor. `ReplayTab` coordinates playback, controls, and the chart's viewport
mode. The chart adapter owns the visual transform, x-slot mapping, y-axis
autoscale, and price/volume/marker drawing. A single viewport calculation must
drive all layers; independent time-axis ranges must not drift. This design
does not require a new dependency or change a persisted record format.

Invalid/empty results keep their existing structured error handling. A
viewport operation with no reached bars is a no-op; extreme wheel input is
clamped. Repeated zoom/pan/resize/seek sequences must remain finite and leave
at least one reached candle visible. Chart failures must not be replaced with
fabricated OHLCV or an apparently successful blank result.

## Acceptance and verification

1. Add failing focused Qt tests for the current all-prefix compression and
   two-screen composition. Assert first/long-prefix candle pixel widths,
   shared x centers, one time-label row, relative price/volume proportions,
   and independently correct price/volume y scales.
2. Test positive, negative, and boundary behavior for Follow latest, manual
   pan, wheel/button zoom, Reset, Fit reached bars eligibility, seek/rewind,
   Hourly/Daily switching, and resize. Include a 23,777-bar prefix, one/two
   bars, missing-hour inputs, flat prices, large/zero volume, and multiple
   same-bar fills. Assert no data truncation and no future candle/fill reveal.
3. Compare chart adapter OHLCV, timestamps, volume and marker mappings against
   the saved AAPL result. Inspect actual app screenshots and interactions at
   three window sizes, early/middle/terminal Hourly and Daily positions, one
   wheel gesture, horizontal drag, and return to Follow latest.
4. Update the owning specifications and Issue 10 plan only when the approved
   behavior is implemented or when clearly labelled as pending. Run focused
   tests, `./RunTest.sh`, full non-fast `./RunQuality.sh` on exact committed
   HEAD, final review, then PR/CI/merge under repository governance. Passing
   tests alone is not visual desktop acceptance.
