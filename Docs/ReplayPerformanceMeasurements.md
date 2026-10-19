# Replay Performance Measurements

These measurements exercise the production immutable-snapshot, Results, and
result-backed Replay paths. They are reproducible evidence for the current
10,000-hourly-bar and 1,000-catalog-entry envelope, not machine-independent
latency guarantees.

## 2026-09-02 baseline

- Host: Apple arm64, macOS 14.6.1.
- Compiler: Homebrew Clang 18.1.8.
- Build: `qt-dev`.
- Replay fixture: 10,000 hourly bars and 10,000 persisted post-slice portfolio
  records; the visible frame window is capped at 500.
- Catalog fixture: 1 valid and 999 unavailable `.bteresult` entries. This
  proves unavailable files do not block the valid entry.

Observed values from GoogleTest XML properties:

| Measurement | Observed |
|---|---:|
| Result validation, data reopen, and hourly frame construction | 192 ms |
| Indexed seek from first to final frame | less than 1 µs |
| Maximum-speed traversal of 9,999 already-open frames | 82 µs |
| Peak resident set for the complete 10,000-bar fixture process | 55,552 KiB |
| List and validate 1,000 catalog entries | 64 ms |

The resident-set value includes fixture generation, SQLite, immutable snapshot
building, GoogleTest, Qt Core, and the open Replay model. It is intentionally a
whole-process peak rather than a misleading attribution to one allocation.

## 2026-10-17 exact snapshot read reuse

- Host: Apple arm64, macOS 14.6.1.
- Compiler: Apple Clang 16.0.0.16000026.
- Build: `qt-dev`, CMake 4.0.1, Qt 6.9.1, SQLite 3.53.4.
- Fixture: the registered `ResultReplayTest.tenThousandHourlyFramesUseIndexedSeekAndBoundedVisibleWindow` test creates 10,000 hourly bars and 10,000 persisted portfolio records. Fixture generation and result persistence are outside the measured interval; `openMilliseconds` measures `ResultReplay::open`.
- Revisions: before `4750042`; after `52621a1` (local `main`, containing `41ee2cc`'s exact snapshot read reuse).
- Method: three independent process runs per revision, using the same binary configuration and checked-in test. Raw `openMilliseconds` samples are shown below; the median improved from 220 ms to 185 ms (15.9%). This is a local comparison, not a latency guarantee.

| Revision | Raw open samples (ms) | Median (ms) |
|---|---:|---:|
| Before `4750042` | 220, 222, 219 | 220 |
| After `52621a1` | 185, 184, 187 | 185 |

Reproduce each sample with a fresh output path for the GoogleTest XML:

```bash
Output/qt-dev/Tests/bte_result_replay_tests \
  --gtest_filter=ResultReplayTest.tenThousandHourlyFramesUseIndexedSeekAndBoundedVisibleWindow \
  --gtest_output=xml:/tmp/replay-performance.xml
```

Read the `openMilliseconds` property from the XML. Run once on each checked-out
revision; repeat in separate processes to collect independent samples.

## Reproduce

```bash
cmake --build --preset qt-dev \
  --target bte_result_replay_tests bte_results_tests --parallel

Output/qt-dev/Tests/bte_result_replay_tests \
  --gtest_filter=ResultReplayTest.tenThousandHourlyFramesUseIndexedSeekAndBoundedVisibleWindow \
  --gtest_output=xml:/tmp/bte-replay-performance.xml

Output/qt-dev/Tests/bte_results_tests \
  --gtest_filter=ResultStoreFixture.thousandEntryCatalogKeepsValidResultAvailableAndSortsDeterministically \
  --gtest_output=xml:/tmp/bte-catalog-performance.xml
```

Read the `openMilliseconds`, `seekMicroseconds`,
`maximumPlaybackMicroseconds`, `peakResidentKibibytes`, and
`listMilliseconds` properties from the generated XML. Re-run on the same host
and build preset when comparing a future change.
