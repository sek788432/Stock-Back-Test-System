# Replay Chart Viewport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. The user selected inline `$implement` execution; do not delegate without authorization.

**Status (2026-10-09):** Approved for inline implementation. The viewport,
chart and controls exist, and focused density/Follow latest regressions pass;
the task checkboxes below are the original execution script, not a reliable
record of acceptance. They remain unchecked until their complete evidence and
final gates are reconciled. Issue 10's dated ledger is the current status
source. A 23,777-bar seek exposed an unmeasured UI stall, and fullscreen
resize left unused chart space until Reset zoom; final desktop acceptance is
open.

**Goal:** Show Replay as one readable price-and-volume K-line chart whose candles remain legible during long playback, with independent navigation and exact saved-result markers.

**Architecture:** A small Frontend viewport model owns ordered-bar-slot range, density, and Follow latest/Review history state independently of the immutable replay cursor. The existing Qt Charts development adapter draws price and volume as one visually continuous chart with a single bottom UTC time-label row; one viewport calculation drives both chart layers and markers. `ReplayTab` owns playback and wires navigation controls, without modifying Result data or executing trading logic.

**Tech Stack:** C++20, Qt 6.8+ Widgets/Charts, CMake, Qt Test. No new dependency or persisted-format change.

**Spec:** [`ReplayChartViewportDesign.md`](../Specs/ReplayChartViewportDesign.md), refined by [`BacktestReplayProduct.md`](../Specs/BacktestReplayProduct.md) and [`FrontendQt.md`](../Specs/FrontendQt.md).

## Global Constraints

- The saved `.bteresult` identity, immutable selected OHLCV rows, timestamps, fill prices/times, and canonical hash remain unchanged.
- Hourly and UTC Daily each render one candle and one volume column per actual selected bar; missing hours, weekends, and closed days never create synthetic candles. Ordered observed bars have equal visual slots; labels and tooltips expose real UTC timestamps.
- Playback reveals only bars/fills through the current replay cursor. All reached bars remain navigable; the 23,777-hourly-bar result is not truncated or automatically fit into subpixel candles.
- Default slot target is 22–26 px (24 px currently), with a roughly 16–20 px
  candle body. Price receives approximately 80–85% and volume 15–20% of the
  plot height; final real inspection at 1024×768, 1200×800, and 1440×900 is
  still required.
- Price autoscale uses only visible candle high/low; volume autoscale uses only visible volume and zero. Marker geometry cannot enter either data scale.
- Preserve the existing chart-first Replay layout and the development-only status of Qt Charts. The project-owned `QPainter` release chart is a separate accepted direction, not a new dependency in this plan.
- Follow `Docs/Governance/AGENTS.md`, `Docs/DefinitionOfDone.md`, `.agents/skills/implement/SKILL.md`, and the required TDD plus five C++ skills. Do not start a later slice while an applicable check is red.

## Preflight and evidence classification

- [ ] Record fixed base `84e3a135b18bdddb8134713df0b6896be0cc1a37` (verify against `origin/main` again before submission), current branch/HEAD, and the three pre-existing dirty Task-24 UTC-label files. Preserve their changes; establish a green baseline or resolve any failure inside the first affected slice, not by discarding them.
- [ ] Verify `Docs/Specs/CiDevFlow.md` and `.github/workflows/ci.yml`: all-tests, full-tree standards, whole-tree static analysis, changed-code coverage (98% line/90% branch), both sanitizer jobs, and merge-gate are **implemented / merge-blocking**. `./RunTest.sh` and full `./RunQuality.sh --base <fixed-base> --head HEAD` are the checked-in local pre-CI workflow. Semantic mutation checks and the eventual `QPainter` release adapter are **planned / not merge-blocking**; actual desktop interaction and hosted PR/CI are **external / unverified** until performed.
- [ ] Read all five project C++ skills, `tdd`, `review`, relevant specs/decisions, and the checked-in build commands before modifying C++. Treat the existing uncommitted UTC-label repair as user work and include it only with its own test/evidence in an authorized coherent checkpoint.

## Review Focus

1. Reached-prefix performance at 23,777 bars: advancing in Follow latest must not rescan or recreate the complete timeline every frame; Task 2/4 tests measure repeated advancement and preserve the full count.
2. Sparse or missing-hour data: adjacent *observed* candles remain equally spaced while UTC labels/tooltips remain truthful; Task 2 tests an absent hour and UTC-day transition.
3. Long pause followed by pan/zoom and replay: playback cursor may advance without pulling Review history back to latest; Task 3/4 tests this sequence.
4. One-bar, flat-price, zero-volume, and huge-volume inputs: no empty/NaN axis or giant candle body; Task 1/2 tests literal geometry and ranges.
5. Rewind, timeframe switch, resize, and same-bar multi-fill: no future bars/fills and no drifting marker/volume centers; Task 2/4 tests these transitions.

---

### Task 1: Independent ordered-bar viewport state

**Files:** Create `Src/Frontend/Include/Bte/Frontend/ReplayBarViewport.h`
and `Src/Frontend/Charts/ReplayBarViewport.cpp`; register `.cpp` in
`Src/Frontend/CMakeLists.txt`; create
`Tests/Unit/Frontend/UnitTest_ReplayBarViewport.cpp` and register
`bte_frontend_viewport_tests` in `Tests/CMakeLists.txt`.

**Interfaces:** `struct ReplayVisibleSlots { std::size_t first; std::size_t end; };` with half-open `[first,end)`; `class ReplayBarViewport` has `void setReachedCount(std::size_t)`, `void setPlotWidth(double)`, `void zoom(double factor, double anchorFraction)`, `void pan(double slotDelta)`, `void followLatest()`, `void reset()`, `[[nodiscard]] ReplayVisibleSlots visibleSlots() const`, `[[nodiscard]] double slotWidth() const`, `[[nodiscard]] bool isFollowingLatest() const`, and `[[nodiscard]] bool canFitReachedBars() const`. `fitReachedBars()` returns `bool` and leaves state unchanged when illegible. No Qt or backend dependency is needed for this state model.

- [ ] **Step 1 — RED:** Write positive/negative/boundary tests with worked literal expected ranges: empty no-op; one reached candle centered at normal width; 2 and 23,777 reached bars; Follow latest slides rather than widens; pointer-anchored zoom; center-button zoom; pan clamps and enters Review history; new bars do not steal a manually reviewed range; reset returns to default; Fit rejects a sub-legible range without mutation; zero/negative/extreme wheel factors remain finite.
- [ ] **Step 2 — verify RED:** Configure/build the new test target, then run `ctest --preset qt-dev -R '^bte_frontend_viewport_tests$' --output-on-failure`; the new behavior must fail before implementation (a compile failure for missing interface is acceptable for the first test only).
- [ ] **Step 3 — GREEN:** Implement the minimal state machine and clamp computations. A 22–26 px default slot target and a positive minimum legible slot are explicit constants in this class; the viewport retains at least one reached bar when nonempty. No bar data or fill data are stored here.
- [ ] **Step 4 — gate:** Run focused viewport tests, `./RunTest.sh`, and affected formatting/static-analysis checks prescribed by `cpp-static-analysis`. Review Task 1 against DoD; commit only Task 1 when green and authorized.

### Task 2: One coherent chart coordinate and readable presentation

**Files:** Modify `Src/Frontend/Charts/QtChartsCandlestickView.cpp`, `Src/Frontend/Include/Bte/Frontend/QtChartsCandlestickView.h`, and `Tests/Unit/Frontend/UnitTest_QtChartsCandlestickView.cpp`; Task-24 UTC-label changes in these files must be preserved and tested.

**Interfaces:** The adapter owns `ReplayBarViewport`. Public `candleCount()`/`volumePointCount()`/`markerCount()` remain full reached-prefix counts. Add `[[nodiscard]] ReplayVisibleSlots visibleSlots() const`, `[[nodiscard]] bool isFollowingLatest() const`, `void followLatest()`, `[[nodiscard]] bool fitReachedBars()`, and `[[nodiscard]] bool canFitReachedBars() const`. Existing `setBarWindow`, `appendBar(s)`, `setMarkers`, `zoomIn/Out`, `resetZoom`, and `setDailyTimeframe` remain callable.

- [ ] **Step 1 — RED:** Add chart tests proving 1, 2, sparse, and 23,777 bars keep distinct nonzero-width candle bodies and full `candleCount()`; a 23,777-bar terminal frame shows a bounded visible slot range instead of all 23,777 at once. Assert same x center for candle/volume/marker at multiple slots, exactly one visible bottom UTC-label row, no separate volume screen framing, and an approximately 80–85% price/15–20% volume split at the three specified window sizes. Use literal OHLCV/UTC expectations, not the production mapping algorithm.
- [ ] **Step 2 — verify RED:** `cmake --build --preset qt-dev --target bte_frontend_chart_tests --parallel`; `ctest --preset qt-dev -R '^bte_frontend_chart_tests$' --output-on-failure` must fail for the old all-prefix range/two-screen behavior.
- [ ] **Step 3 — GREEN:** Replace elapsed-time x positioning with ordered bar slots shared by candle, volume, and fill-glyph layers; keep original UTC timestamps in adapter metadata/tooltips and generate one bottom UTC label row from the corresponding observed bars. Use one viewport-derived x range for both frameless Qt Charts plots. Recompute y ranges from only visible OHLC and volume; keep marker pixels outside financial autoscale. Preserve the dirty Task-24 UTC-label intent while converting its label source to slot-to-UTC mapping.
- [ ] **Step 4 — edge RED/GREEN:** Add flat/high/zero-volume and extreme-price tests, negative invalid-bar/invalid-marker checks, same-candle multiple fills, resize, and Hourly/Daily label tests; fix any discovered mapping/scale defect within this slice. Compare literal series OHLCV and marker fill details against inputs after pan/resize.
- [ ] **Step 5 — gate:** Run focused chart tests and `./RunTest.sh`; run affected formatting/static-analysis checks and a reproducible before/after long-prefix append measurement. Review DoD and commit only this green slice when authorized.

### Task 3: Zoom, pan, Follow latest, Reset, and Fit actions

**Files:** Modify chart adapter/header plus `Src/Frontend/Replay/ReplayTabSections.h`, `Src/Frontend/Replay/ReplayTabSections.cpp`, `Src/Frontend/Replay/ReplayTab.cpp`, `Tests/Unit/Frontend/UnitTest_QtChartsCandlestickView.cpp`, and `Tests/Unit/Frontend/UnitTest_ReplayTab.cpp`.

**Interfaces:** Chart methods `zoomIn()`, `zoomOut()`, `resetZoom()` now change horizontal slot density only; wheel zoom is pointer-anchored and chart drag calls viewport `pan`. Add `Q_SIGNAL void viewportModeChanged(bool followingLatest)` to the chart, plus the Task-2 Follow/Fit methods. Controls add `QToolButton *followLatestButton` and `QToolButton *fitReachedBarsButton` with object names `replayFollowLatestButton` and `replayFitReachedBarsButton`; ReplayTab updates visible/enabled state from chart state.

- [ ] **Step 1 — RED:** Qt tests send wheel and drag events to the actual chart viewport. Assert zoom changes visible slot count without changing candle/marker counts or result identity, pointer anchor remains under cursor, a horizontal drag enters Review history, subsequent append leaves reviewed slots stable, Follow latest catches up, Reset restores default density and following, Fit is disabled/rejected for 23,777 bars, and repeated extreme input/resize never empties a nonempty view. Add keyboard-focus/accessibility-name checks for new actions.
- [ ] **Step 2 — verify RED:** Build chart/frontend targets; `ctest --preset qt-dev -R '^bte_frontend_(chart_)?tests$' --output-on-failure` fails on old chart navigation.
- [ ] **Step 3 — GREEN:** Route wheel/button/drag actions through one viewport state instead of `QChart::zoom/scroll`, update both layers from the same range, and wire Follow/Fit controls. Show an explicit Review history state while playback can continue; a navigation action never calls Replay seek.
- [ ] **Step 4 — gate:** Focused tests, `./RunTest.sh`, affected format/analysis, and task self-review; commit the green slice only.

### Task 4: Replay cursor, timeframe, and marker integration

**Files:** Modify `Src/Frontend/Replay/ReplayTab.cpp`, chart adapter as needed, `Tests/Unit/Frontend/UnitTest_ReplayTab.cpp`, `Tests/Unit/Frontend/UnitTest_ResultReplayTab.cpp`, and `Tests/Unit/Bindings/UnitTest_ResultReplay.cpp` only if the existing public seam needs additional regression coverage. No Results schema or Engine changes.

**Interfaces:** `renderResult`/preview presentation pass only the reached prefix into the chart. A forward batch appends its new bars; rewind or result/timeframe switch replaces the reached prefix and resets density/follow as specified. Pan/zoom never calls `ResultReplay::seek`; cursor changes never discard immutable timeline rows.

- [ ] **Step 1 — RED:** Exercise an actual saved Result through ReplayTab: first candle only; Step/Play/max-speed batch reveals new bars/fills with unchanged candle width; Back/seek backward hides all future bars/fills/rows; pan Review history then advance keeps reviewed range; Follow latest jumps to reached tail; Hourly↔Daily preserves the existing UTC-date cursor mapping and resets viewport; result switch clears stale glyphs. Include missing-hour and multi-fill same-bar cases.
- [ ] **Step 2 — verify RED:** Build/run `bte_result_replay_frontend_tests`, `bte_frontend_tests`, and `bte_frontend_chart_tests` through `ctest --preset qt-dev -R '^(bte_result_replay_frontend_tests|bte_frontend_tests|bte_frontend_chart_tests)$' --output-on-failure`.
- [ ] **Step 3 — GREEN:** Update ReplayTab/chart boundary for incremental forward work and exact replacement on rewind. Keep chart slot metadata and trade rows synchronized with persisted frames. Avoid rebuilding all 23,777 candles/markers/log rows on each advanced frame; use a reproducible early/middle/terminal measurement and document the hot-path result.
- [ ] **Step 4 — gate:** Run focused integration tests, `./RunTest.sh`, affected analysis/format, and self-review; commit only when green.

### Task 5: Specification truth, actual-app verification, final gates and review

**Files:** Modify `Docs/Specs/ReplayChartViewportDesign.md` status, `Docs/Specs/BacktestReplayProduct.md`, `Docs/Specs/FrontendQt.md`, `Docs/Issue10DeterministicReplayFixturePlan.md`, and this ledger only to reflect observed behavior. Do not present separately pending Issue-10 tasks as complete.

- [ ] **Step 1 — docs:** Update the owning descriptions from two framed panes/all-prefix fit to the implemented unified presentation and state model. Correct known-gap/status tables, cross-links, and remaining Task-24/Issue-10 checklist items truthfully. Validate relative links and `git diff --check`.
- [ ] **Step 2 — desktop:** Launch the actual rebuilt app through `Launch.sh`. Open the saved AAPL Result and compare sampled Hourly/Daily OHLCV, UTC labels, volume, and persisted Buy/Sell fills with source records. Inspect 1024×768, 1200×800, 1440×900 at first/middle/terminal positions; test wheel zoom, horizontal drag, Follow latest, Fit eligibility, resize, Pause/Play/Step/Back, and result/timeframe switching. Record counts, dimensions, screenshots/evidence paths, and any unavailable interaction honestly; no synthetic fixture alone can satisfy this step.
- [ ] **Step 3 — exact submission commit:** Review every diff hunk and make a cohesive conventional commit. Run `./RunTest.sh` and full non-fast `./RunQuality.sh --base 84e3a135b18bdddb8134713df0b6896be0cc1a37 --head HEAD` on that exact committed HEAD. If either fails, fix the current slice and rerun; never request CI from a red revision.
- [ ] **Step 4 — required review:** Use project `review` skill against the fixed base, resolve every blocking finding, and rerun exact-HEAD gates after fixes. Verify applicable Definition of Done items. Only then proceed to the separately authorized PR → hosted CI → merge workflow; attach any created PR and report external checks without inventing pass results.

## Spec coverage self-review

Task 1 owns viewport state, follow/pan/zoom/fit clamping and no-empty boundaries. Task 2 owns one visual frame, equal actual-bar slots, UTC labeling, autoscale, OHLCV/marker geometry and size ratios. Task 3 owns user navigation controls and Review history signaling. Task 4 owns result cursor, progressive reveal, timeframe and performance-sensitive synchronization. Task 5 owns real-desktop acceptance, documentation truth, exact-HEAD gates, independent review, and PR handoff. No acceptance item in the source design is intentionally omitted.
