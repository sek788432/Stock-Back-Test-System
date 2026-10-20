# Issue 10 Canonical Integer Framing Evidence

## Scope and limits

The bounded repair writes each schema-2 integer frame directly into the existing
canonical byte stream: tag, unsigned big-endian length 8, signed two's-complement
big-endian value. It removes the temporary per-integer string without changing
the schema, functional inputs, SHA-256 implementation, data validation,
cancellation, timeout or CI gates. It does not repair R1–R3 persistence safety.

The independent oracle below uses Python's standard `struct` and `hashlib`,
without calling C++ framing/digest code or reading an actual result hash. It
reconstructs the literal two-row `ResultStoreFixture` immutable source, its
one-row segments and manifest, then schema-2 frames in the normative order from
[EngineReplayPnL.md](../Specs/EngineReplayPnL.md#81-implemented-schema-2-canonical-hash-framing).
The C++ test pins the generated snapshot ID and both expected hashes.

The extra fourth portfolio record is a serialization boundary fixture, not an
Engine accounting claim. It covers signed 64-bit minimum/maximum, -1, -256,
zero versus absent optionals, and a UTF-8 combining accent. Completed and
Interrupted terminal states cover present and absent summary fields. Existing
ResultStore validation tests remain responsible for rejecting invalid records.

Before optimization, `schemaTwoHashMatchesIndependentLiteralOracle` passed in
26 ms. A temporary mutation XORed each integer's encoded value with one; the
writer and reopened-reader hashes both disagreed with the literal expectations
for both statuses (four failed assertions, exit 1). This detects the case where
writer and reader consistently implement the same wrong framing. The mutation
was removed before the optimization and is not part of the delivered diff.

## Independent worked oracle

Run the following Python 3 code. Expected output is snapshot
`bf5e2533584d2e65312e8d15c03c11c10a4118bfca341ef806a04b38e9596ec6`,
Completed byte count 1857 / hash
`b8b436a6dd7fc99a86e40fa2ea04ebe0a3633bf303927147d8cd98f6f0b1e3d7`,
and Interrupted byte count 1841 / hash
`5d16a7ba640f51cf7b6dbcc22a1e9299ca230ce66b0c83cf4ced1d1014192978`.

```python
import hashlib
import struct


def integer(value):
    return struct.pack(">q", value)


def frame(tag, value):
    payload = b"" if value is None else (integer(value) if isinstance(value, int) else value.encode("utf-8"))
    return tag.encode("ascii") + struct.pack(">Q", len(payload)) + payload


times = [1704150000000, 1704153600000]
rows = [(times[0], 100000000000, 102000000000, 99000000000, 101000000000, 1200000000),
        (times[1], 101000000000, 103000000000, 100000000000, 102000000000, 2300000000)]
segments = [hashlib.sha256(b"BTEDATA1" + struct.pack(">Q", 1) + struct.pack(">Q", 3) + b"SYN" + struct.pack(">Q", 1) + struct.pack(">6q", *row)).hexdigest() for row in rows]
manifest = "BTE-SNAPSHOT-MANIFEST-V1\nprofile=vendorExtendedHours\ntimeframe=ohlcv-1h\ncalendarHash=" + "a" * 64 + "\nsplitManifestHash=" + "b" * 64 + "\n"
for ordinal, (segment, timestamp) in enumerate(zip(segments, times)):
    manifest += f"segment={ordinal}|SYN|{segment}|1|{timestamp}|{timestamp}\n"
snapshot = hashlib.sha256(manifest.encode("ascii")).hexdigest()
header = [("v", 2), ("u", "SYN"), ("s", times[0]), ("e", 1704157200000), ("c", 100000000), ("i", "starter"), ("h", "c" * 64), ("n", "fixed-point-v1"), ("a", "utcCalendarDayV1"), ("d", snapshot), ("d", "a" * 64), ("d", "b" * 64), ("d", "ohlcv-1h"), ("d", "vendorExtendedHours")]
for ordinal, (segment, timestamp) in enumerate(zip(segments, times)):
    header += [("o", ordinal), ("y", "SYN"), ("g", segment), ("g", segment), ("r", 0), ("r", 1), ("t", timestamp), ("t", timestamp)]
# q, t, y, f, x, then optional 1/2/3/4/5/6/p/7 and text.
records = [
    [0, times[0], "SYN", 2, 0, None, None, None, 100000000, 0, 100000000, 0, 0, ""],
    [1, times[1], "SYN", 1, 1, 10, 101010000000, 1010100000, None, None, None, None, None, ""],
    [2, times[1], "SYN", 2, 0, None, None, None, 98989900000, 1020000000, 100009900000, 9900, 10, ""],
    [3, times[1], "SYN", 2, 0, None, None, None, -(2**63), 2**63-1, -1, -256, 0, "boundary cafe\u0301"],
]
body = b"BTE-CANONICAL-RESULT\0" + b"".join(frame(tag, value) for tag, value in header)
for record in records:
    body += b"".join(frame(tag, value) for tag, value in zip("qtyfx123456p7m", record))
print("snapshot", snapshot)
print("segments", segments)
for status, summary in [(1, [100009900000, -256]), (4, [None, None])]:
    encoded = body + b"".join(frame(tag, value) for tag, value in [("z", status), ("m", "oracle"), ("8", summary[0]), ("9", summary[1])])
    print(status, len(encoded), hashlib.sha256(encoded).hexdigest())
```

## Local performance measurement

Measured on 2026-10-18, macOS 14.6 arm64, Apple LLVM 16, Qt 6.9.1, existing
`coverage` preset (`--coverage -O0 -g`), offscreen platform. Each sample starts
a fresh process with the unchanged 23,777-bar / 6,793-fill synthetic fixture.
No other source changes are included in the comparison. Baseline source is
`cc18ab85cea56c9e952cd7b52ba14f2858a4540d`.

```bash
QT_QPA_PLATFORM=offscreen Output/coverage/Tests/bte_result_replay_frontend_tests longRangeSeekPresentsEveryReachedCandleAndFill
```

Three sequential baseline runs: load 1196 / 1035 / 1055 ms;
seek 2715 / 2580 / 2584 ms; all exited zero.

Three sequential optimized runs: load 916 / 952 / 926 ms;
seek 2550 / 2549 / 2680 ms; all exited zero. Median local load changed
from 1055 to 926 ms (12.2% lower); seek is outside the changed framing path.
The Linux coverage regression remains open until fresh required remote checks
pass; local performance does not establish a cross-machine latency guarantee.

The first targeted rebuild emitted stale GCDA counter merge warnings during
GoogleTest discovery. Only generated `ResultStore.cpp.gcda` and
`UnitTest_ResultStore.cpp.gcda` files were moved to an owned temporary backup
before collecting clean after samples. These focused measurements do not
establish aggregate coverage or Linux remote acceptance. Required exact-head
full tests and complete Quality remain a separate final gate.

## Focused verification

- `Output/coverage/Tests/bte_results_tests`: 58/58 passed (7.42 seconds),
  including the literal oracle after optimization.
- Rebuilt `qt-dev` Results and Result Replay frontend targets; Results 58/58
  passed (7.55 seconds) and complete Replay frontend 20/20 passed.
- `git diff --check` and the working-tree project standards audit against
  `cc18ab8` passed. The independent Python code reproduced both literals.
- Full exact-commit `RunTest.sh`, complete `RunQuality.sh`, sanitizer runs and
  remote acceptance remain for the coordinated submission; focused passes do
  not establish whole-branch acceptance.

A direct two-file clang-tidy attempt used `Output/analysis` for the test TU,
which is absent from that database. It failed on missing test-hook includes
and resulting macro diagnostics; it is not a passing analyzer result. The
checked-in `RunStaticAnalysis.py` intentionally analyzes project `Src` units.
The production TU is checked with its actual analysis compilation command;
full coordinated Quality remains required. No check or suppression was changed.

The corrected production-only clang-tidy invocation exited zero with no
reported project finding (`ResultStore.cpp`, `Output/analysis` database).
