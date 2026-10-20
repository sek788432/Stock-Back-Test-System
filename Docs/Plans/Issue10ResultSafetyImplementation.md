# Issue 10 Result Safety Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans for the Results/Data tasks; Engine and Bindings workers implement their assigned seams independently.

**Current status (2026-10-19):** R1–R3 implementation and focused local
regressions are in the consolidation worktree. Full exact-commit Quality,
sanitizers, remote PR checks, review, and native real-data acceptance remain
open; those gates must pass before this plan is considered accepted.

**Goal:** Implement the approved Result ownership, retention and committed-prefix recovery protocol without changing schema-2 canonical identity.

**Architecture:** Results owns a concrete RAII SQLite lease per opaque Result ID. Lease acquisition precedes short retention transactions and artifact transactions. Recovery preserves unknown artifacts and surfaces immutable typed notices rather than guessing ownership.

**Tech Stack:** C++20, SQLite 3.53.4, existing Core Result, Python standard-library process tests.

**Spec:** [ResultPersistenceSafetyDesign.md](../Specs/ResultPersistenceSafetyDesign.md), explicitly approved by the maintainer with implementation authorization in the current conversation.

## Global Constraints

- Preserve schema 2, canonical frames, successful hashes and the existing module graph.
- Lease databases use DELETE rollback journaling, synchronous EXTRA, protocol 1 and zero busy timeout; retain lease files after release.
- Legacy unleased Staging and removal sidecars remain byte-identical and pinned.
- Never hold an application registry mutex over disk I/O; lock order is lease, short retention transaction, artifact transaction.
- All terminal-tail records and final status/summary/hash commit atomically.

## Review Focus

- Concurrent process opens during empty writer initialization must skip without touching artifact bytes.
- A terminated process with zero or multiple committed bars must recover only a complete prefix, once.
- Alias paths must share SQLite ownership, with no PID/expiry-based stealing.
- Initialization/import retention faults must expose no unpinned Staging/Draft/Result.
- Corrupt leases/prefixes and initialization orphans must preserve valid neighboring Results and retained references.

## Task 1: Lease and retention visibility

Files: `Src/Backend/Results/Private/ResultStore.cpp`, `Src/Backend/Core/Include/Bte/Core/Result.h`, Results tests and private process helper.

- [x] Add separate-process writer/recovery tests covering empty live writers, committed prefixes, alias paths, and recovery after process death.
- [x] Add `ErrorCode::resourceBusy`; implement concrete SQLite lease with collision-safe hard-link installation of a fully initialized private database, then BEGIN EXCLUSIVE before artifact creation.
- [x] Replace the process-wide lifetime registry with SQLite ownership; pin distinct source segments and revalidate before creating Staging/import copy.
- [x] Acquire ownership around all lifecycle mutations, retaining small lease tombstones.
- [x] Run Results and process tests; verify busy operations have no forbidden effects.

## Task 2: Conservative recovery and notices

Files: `ResultStore.h/.cpp`, `SegmentRetention.h/.cpp`, Results tests.

Interfaces: `ResultRecoveryReason`, `ResultRecoveryNotice{resultId, reason, diagnostic}`, `const std::vector<ResultRecoveryNotice>& ResultStore::recoveryNotices() const noexcept`, `Result<std::vector<std::string>> SegmentRetentionStore::references(const std::string&) const`.

- [x] Add regression assertions for byte-identical legacy staging/removals, invalid trailing events, missing data, missing pins, corrupt identity, orphan metadata and idempotency.
- [x] Nonblocking acquire before any staging read; busy skips, unknown files preserve and report, storage-root failure/cancellation propagates.
- [x] Require zero records or a complete bar prefix ending with portfolio; pin/revalidate before Interrupted Draft promotion. Preserve existing terminal status.
- [x] Detect unlocked valid lease IDs with pins and no artifact without releasing references or removing lease metadata.
- [x] Run Results/Data tests and owned-child crash tests.

## Task 3: Atomic terminal records

Files: `ResultStore.h/.cpp`, Results tests; Engine worker owns callers.

Interface: `Result<FinalizedResult> ResultWriter::finalizeDraft(RunStatus, const FinalSummary&, const std::string&, std::span<const CanonicalRecord>)`.

- [x] Add fault regression proving terminal tail and summary/status/hash remain all-or-nothing.
- [x] Extract shared canonical batch validation/insertion used by append and terminal finalization, preserving linear record processing.
- [x] Keep existing three-argument API forwarding an empty tail; use one artifact transaction for tail plus metadata/hash.
- [x] Run literal canonical oracle and complete Results/Engine integrations.

## Task 4: Submission

- [ ] Review full effective diff, update owning specs/decision status with coordinated workers and preserve all existing performance/accessibility fixes.
- [ ] Build and run all registered tests; run complete non-fast Quality and ASan/TSan on exact committed HEAD through the coordinating root.
- [ ] Commit conventional focused changes; no push until final gates pass.
