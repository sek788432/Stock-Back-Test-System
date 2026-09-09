# K-line Replay Roadmap

The canonical contracts are [Architecture](Specs/Architecture.md),
[Data Layer](Specs/DataLayer.md), [Engine, Replay, P&L, and Results](Specs/EngineReplayPnL.md),
and [Backtest and Replay Product](Specs/BacktestReplayProduct.md). The
[canonical result storage decision](Decisions/ImportantDecisions.md#canonical-result-storage-and-lifecycle)
owns Results identity, hashing, durability, recovery, and retained data.

## Delivery and prerequisite traceability

[Issue #10: deterministic K-line replay fixture](https://github.com/sek788432/Stock-Back-Test-System/issues/10)
owns the final small offline fixture through production snapshot, `.bteresult`,
catalog, and Replay seams. It does not own the production prerequisite changes.
The existing implementation is a limited single-symbol path, and the presence
of a fixture does not establish completion of every prerequisite below.

All seven plan tasks travel together in
[PR #68](https://github.com/sek788432/Stock-Back-Test-System/pull/68), with one
semantic commit per task as requested for this integrated delivery. The commit
subjects below remain stable if the branch is rebased or amended.

| Plan task | Prerequisite scope | Implementation commit | Related issue |
| --- | --- | --- | --- |
| 1 | Results ownership, schema/lifecycle decision, SQLite provenance, build wiring, and status documentation | `docs(results): define persisted replay architecture` | #10 delivery prerequisite |
| 2 | Immutable segment construction/reading, exact selection spans, and retention | `feat(data): add immutable backtest data identity` | [#4](https://github.com/sek788432/Stock-Back-Test-System/issues/4), [#11](https://github.com/sek788432/Stock-Back-Test-System/issues/11) |
| 3 | Transactional Results writer/store, catalog, import, recovery, and lifecycle | `feat(results): add transactional backtest result store` | [#19](https://github.com/sek788432/Stock-Back-Test-System/issues/19) is the legacy-storage baseline |
| 4 | Engine-emitted canonical order/fill/portfolio timeline and persisted Backtest integration | `feat(engine): record canonical backtest timelines` | [#12](https://github.com/sek788432/Stock-Back-Test-System/issues/12) |
| 5 | Result-backed Bindings, Hourly/Daily indexed frames, validation, cancellation, and stale-request handling | `feat(bindings): expose persisted result replay` | [#9](https://github.com/sek788432/Stock-Back-Test-System/issues/9) |
| 6 | Backtest-to-Replay navigation and persisted candle/volume/fill/portfolio presentation | `feat(replay): visualize persisted backtest results` | [#8](https://github.com/sek788432/Stock-Back-Test-System/issues/8), [#13](https://github.com/sek788432/Stock-Back-Test-System/issues/13), [#18](https://github.com/sek788432/Stock-Back-Test-System/issues/18) |
| 7 | Final offline deterministic production-path fixture | `test(replay): add deterministic K-line replay fixture` | [#10](https://github.com/sek788432/Stock-Back-Test-System/issues/10) |

| Prerequisite | Current status and evidence | Issue traceability |
| --- | --- | --- |
| Architecture and dependency provenance | **Implemented documentation/build pin:** the living storage decision and root [`vcpkg.json`](../vcpkg.json) fix ownership and SQLite provenance. | Production prerequisite to #10; no dedicated architecture issue is recorded. |
| Exact immutable data selection and retention | **Implemented limited slice:** [Data](../Src/Backend/Data/CMakeLists.txt) builds immutable snapshot/segment support. Release calendar/split and full release delivery remain **Planned / Blocked** as specified by Data Layer. | [#4: CSV-backed BarStream](https://github.com/sek788432/Stock-Back-Test-System/issues/4) is the historical baseline. [#11: read-only DuckDB DataSource](https://github.com/sek788432/Stock-Back-Test-System/issues/11) is closed and does not authorize a DuckDB release runtime. Dedicated immutable-snapshot prerequisite tracking remains to be recorded. |
| Transactional Results store and authoritative Engine timeline | **Implemented limited slice:** [Results](../Src/Backend/Results/CMakeLists.txt) and [Engine](../Src/Backend/Engine/CMakeLists.txt) persist supported records; import and complete canonical families remain **Planned**. | [#12: portfolio snapshots and trade markers](https://github.com/sek788432/Stock-Back-Test-System/issues/12) is related engine work. [#19: persist replay result snapshots](https://github.com/sek788432/Stock-Back-Test-System/issues/19) is a closed historical JSON-summary task; it is not canonical `.bteresult` acceptance. Remaining storage prerequisites require separate tracking. |
| Result catalog/open and presentation-only Replay | **Implemented and verified for the accepted single-symbol scope:** [Bindings](../Src/Bindings/CMakeLists.txt) validates exact spans, cancellation, compatibility, stale requests, diagnostic truncation, indexed seeking, and Hourly/Daily presentation. | [#9: wire ReplaySessionVm to backend replay](https://github.com/sek788432/Stock-Back-Test-System/issues/9) is the closed bar-playback baseline. |
| Result navigation, candles, volume, fills, and portfolio | **Implemented and verified for the accepted single-symbol scope:** [Frontend](../Src/Frontend/CMakeLists.txt) and [App](../Src/App/CMakeLists.txt) cover accessible navigation, partial UTC days, bounded rendering, synchronized controls, fills, and authoritative portfolio display. | [#8: candlestick chart](https://github.com/sek788432/Stock-Back-Test-System/issues/8), [#13: portfolio snapshots and trade markers](https://github.com/sek788432/Stock-Back-Test-System/issues/13), and [#18: playback controls](https://github.com/sek788432/Stock-Back-Test-System/issues/18) are historical baselines. |
| Final deterministic production-path fixture | **Implemented and verified:** [registered tests](../Tests/CMakeLists.txt) exercise the real immutable snapshot → Results writer → catalog → Replay path twice, assert literal Hourly/Daily frames and independent-run identity/hash behavior, and fail closed under deliberate production mutation. | [#10](https://github.com/sek788432/Stock-Back-Test-System/issues/10), final fixture only. |

Issue links identify actual existing work. PR #68 is an explicitly integrated
delivery and does not retroactively redefine the scope of the historical
issues. Future expansion beyond the accepted single-symbol capabilities needs
its own issue and PR.

## Acceptance evidence

- The deterministic fixture and independent-Backtest integration tests cover
  literal candles, checked volume, stable fill ordering, portfolio checkpoints,
  seek/progress, UTC Daily aggregation, partial and diagnostic boundaries,
  distinct Result IDs, and equal functional hashes.
- [Replay performance measurements](ReplayPerformanceMeasurements.md) document
  reproducible 10,000-frame open/seek/maximum-playback/memory evidence and a
  1,000-entry catalog measurement; visible chart data is capped at 500 frames.
- The complete normal, sanitizer, thread-sanitizer, coverage, and static-analysis
  workflows are the merge gates for PR #68.
- Manual desktop acceptance completed a selectable-strategy Backtest, opened
  its exact promoted Result ID, displayed its five-candle K-line chart, volume,
  four fills, final profit, and advanced one synchronized Replay step.

## Fixture acceptance boundary

The final fixture must assert literal Hourly and UTC Daily candles, volume,
stable fill ordering, post-slice portfolio values, seek/progress, partial and
diagnostic boundaries, and identical replay sequences from an immutable result.
Replay must not invoke Engine or Strategy execution. Distinct Result IDs and
equal canonical hashes for identical Backtests are separate integration
evidence. SQLite physical bytes, wall-clock timing, and local paths are never
the determinism oracle.

Use the [implemented verification workflow](Specs/CiDevFlow.md) and the
[Definition of Done](DefinitionOfDone.md). Remaining target-state checks are
not merge-blocking until CI implements them. A public data-bearing release
remains **Blocked** pending redistribution rights and a verified split manifest.
