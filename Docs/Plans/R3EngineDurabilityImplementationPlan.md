# Recorded Engine Bar Durability Implementation Plan

> For agentic workers: use executing-plans to implement each test/implementation step in order.

**Goal:** Commit every completed Engine bar before exposing progress, and recover only committed prefixes.

**Architecture:** Recorded BacktestExecution owns a ResultWriter; its existing transition computes one bar and appends only the new canonical record delta before advancing presentation. Terminal-only records share the Results atomic terminal transaction. Existing in-memory execution remains available; the synchronous borrowed-writer entry point attaches a nonescaping borrow to the same transition.

**Tech Stack:** C++20, existing Results SQLite writer, GoogleTest, QtTest for queued UI adapters.

**Spec:** Docs/Specs/ResultPersistenceSafetyDesign.md sections 5 and 8, approved by the maintainer on 2026-10-18.

## Global constraints

- Preserve schema 2, canonical framing/records/hash, fixed-point values, public in-memory start and finalizeAndRecord compatibility.
- Do not change ResultStore-owned files; coordinate atomic terminal-tail overload with its owner.
- Per-bar work copies only the new record delta, never cumulative records.
- Storage failure makes that execution permanently terminal and discards uncommitted presentation state; no transition retry.
- No source mutation until the preceding exact-head Quality freeze is released.

## Review focus

- First-bar failure must expose no uncommitted warnings, orders, fills or checkpoint.
- Second-bar commit failure must leave exactly the first committed bar and financial state.
- Cancellation/operational error after committed bars must atomically finalize a diagnostic tail without duplicating committed records.
- A terminal transaction failure must not leave a Running artifact with a separately committed terminal tail.
- Borrowed writer lifetime stays inside runBacktestAndRecord and both recording entry points use per-bar writes.

## Task 1: Recorded owner and durable advance

Files: Backtest.h, Backtest.cpp, UnitTest_RecordedBacktest.cpp.
Interfaces: startRecorded(BacktestRequest, const ResultStore&, const RunDescriptor&, cancellation={}); finalizeRecorded(); existing advance/result.

- Add failing tests for missing recorded factory, one-bar prefix after owner destruction/recovery, first/second append failure with zero/unmutated progress, and identical complete canonical records/hash.
- Run focused Engine tests and capture expected RED before production edits.
- Add owned writer and committed record count; preserve a constant-size scalar checkpoint and resize vectors on failure.
- Append the new bar delta before incrementing nextBar/barsProcessed; mark permanent storage failure and close owned writer on append error.
- Re-run focused tests. Prefix recovery tests depend on Results lease/pin/prefix implementation.

## Task 2: Atomic terminal tails and compatibility routes

Consumes ResultWriter::finalizeDraft(status,summary,reason,std::span<const CanonicalRecord> tail).

- Add failing cancellation, stale-final-mark, append/commit failure, and no-duplicate finalization tests.
- Route finalizeRecorded/finalizeAndRecord through the atomic terminal-tail overload; reject attempts to finalize through a different writer.
- Route store and borrowed runBacktestAndRecord through the recorded transition; a private friend keeps the borrowed writer from escaping its synchronous call.
- Verify existing compatibility failure tests and literal original canonical records/hash.

## Task 3: Draft notices and Bindings

Files: BacktestSessionVm.h/.cpp and UnitTest_BacktestSessionVm.cpp.
Consumes shared Bindings ResultCatalogSnapshot and ResultStore::recoveryNotices().

- Add listBacktestDraftCatalog returning entries/notices; keep existing listBacktestDrafts vector<string> compatibility.
- Add missing-store/cancelled/valid-legacy-sidecar tests; actual frontend display is owned by the UI agent.
- Verify persisted configuration still reaches incremental Engine recording and propagates storage failure.

## Task 4: Crash boundary and regression verification

- Exercise an owned child process that terminates after a completed bar; recover only its canonical prefix, without executing Strategy or appending future records.
- Run Engine, BacktestSessionVm, Replay determinism and focused sanitizer checks, then request coordinated full exact-head RunTest/Quality and process tests.
- Review exact code/test/docs diff and commit only owned files using a conventional message. Do not push until coordinated gates pass.
