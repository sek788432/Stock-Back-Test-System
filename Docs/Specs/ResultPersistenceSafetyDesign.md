# Result Persistence Safety Design

**Status:** R1–R3 are merged in PR #76; PR #81 also passed exact-submitted-head
local Quality, sanitizer, required CI, and CodeQL checks. Native application
acceptance remains open. The maintainer approved the detailed design and R1–R3
implementation on 2026-10-18. Historical predicates, hash-bound CSV timeframe
provenance, and long-range real-data reconciliation are implemented in PR #81;
remaining native checks are listed in the
[Issue 10 remaining-work list](../Plans/Issue10RemainingWork.md).

**Owners:** [Engine/Results](EngineReplayPnL.md), [Data](DataLayer.md),
[Frontend](FrontendQt.md), and the
[storage decision](../Decisions/ImportantDecisions.md#canonical-result-storage-and-lifecycle).

## 1. Intent and current gaps

The user must not lose completed Backtest bars because another app process
opens Results or because execution crashes before Save. Save Result and Save &
Replay still publish the same completed or diagnostic draft, without rerunning
Engine. A recovered interrupted run is never presented as Completed.

At checkpoint `b7e27a9`, Results tracked writers only in process memory,
published Staging before pinning source data, and recorded the Engine timeline
after execution. The current implementation replaces those paths with a
per-Result SQLite lease, retention before visibility, an atomic write for each
completed bar, and recovery limited to the committed prefix. These operational
fields do not change schema-2 canonical identity. Automated delivery gates have
passed; the remaining acceptance gate is native application validation, not
design approval.

## 2. Scope and unchanged contracts

- Retain SQLite 3.53.4, the pinned manifest and existing module dependency graph.
- Keep backend code Qt-independent and SQLite private to Results/Data.
- Keep canonical result schema 2, framing, numeric policy, record order and
  successful-run hashes unchanged. Lease metadata is operational, not canonical.
- Apply the completed-bar protocol to the implemented single-symbol starter
  and selectable Engine paths. Do not claim general multi-symbol execution.
- No new strategy execution, resume-from-crash execution, official session
  calendar, provider integration or financial record families.
- Preserve legacy unleased Staging and report it. Do not auto-delete, move,
  quarantine, finalize or release its references.
- Concurrent supported processes must all use this protocol. It cannot prevent
  an older executable or an unrelated tool that ignores leases from modifying
  files; mixed old/new writers against the same store are unsupported.
- Support local filesystems with SQLite's locking and synchronization semantics.
  Do not claim network-filesystem or faulty-storage power-loss guarantees.

## 2.1 Approaches considered

- A process-local mutex/path registry is insufficient by itself: a second app
  process cannot observe it, and process death erases its state. Retain only a
  short-held in-process registry as a same-process supplement.
- One lifetime transaction on the global catalog database would serialize
  unrelated Result IDs and couple catalog availability to a long Backtest.
- **Selected:** a per-Result SQLite lease file, with a separate short retention
  transaction and per-bar artifact transaction. This isolates unrelated runs
  while allowing SQLite's local-filesystem locks to coordinate processes.

## 3. Writer ownership

Use a private per-result SQLite lease database at
`Leases/<resultId>.sqlite3`, separate from the canonical artifact. A Results-owned
RAII lease holds one connection and a live `BEGIN EXCLUSIVE` transaction using
rollback-journal mode. Creating its versioned metadata and acquiring ownership
finish before Staging is created. The lifetime transaction does not contain
canonical records; completed-bar commits use the artifact's separate connection.

Configure this lease database for rollback-journal `DELETE` mode and
`synchronous=EXTRA`; WAL is not permitted for the lease because it does not
provide the required exclusive-reader exclusion. `EXTRA` also syncs the journal
directory after rollback-journal deletion at commit. Set a zero busy timeout
for lease acquisition. Map both `SQLITE_BUSY` and `SQLITE_LOCKED` to a typed
`resourceBusy` error; never wait while scanning another process's Staging file.
Create a new lease file exclusively, commit its versioned identity metadata,
then acquire the lifetime exclusive transaction before creating any recoverable
artifact. If another process wins the brief post-initialization acquisition
race, this attempt returns busy and creates no artifact. Existing lease
metadata is read only after the exclusive lock is held and validated.

The existing process-local registry may reject a duplicate same-process
acquisition, but its mutex protects only registry lookup/update; it is never
held over disk I/O or for the lease lifetime. It is not the cross-process
authority.

The lease metadata identifies protocol version 1 and its Result ID. It excludes
PID liveness, expiry timers and wall-clock-based stealing. Recovery attempts
ownership non-blockingly: `SQLITE_BUSY` means skip the artifact without reading,
moving or editing it. Other acquisition/validation errors preserve it and
produce a structured notice. An absent lease for an existing Staging artifact
means legacy-unleased, not permission to create a lease and claim ownership.

For an existing lease, acquire the transaction before validating its metadata.
Do not change journal mode or recreate the lease during inspection. Reject a
wrong Result ID/version or unsupported mode. New lease creation uses the same
collision-safe allocation discipline as result artifacts; competing processes
must never overwrite an existing identity.

SQLite specifies that exclusive transactions prevent other connections from
reading in non-WAL modes; WAL does not give that additional read exclusion.
This is why the lease database and artifact WAL transactions are separate.
[SQLite transaction documentation](https://www.sqlite.org/lang_transaction.html)
[SQLite synchronous pragma](https://www.sqlite.org/pragma.html#pragma_synchronous)

Hold the lease through initialization, execution, terminal validation, promotion
and cleanup. Close the artifact connection before releasing the lease. Retain
the lease file after release so every process locks the same file identity;
never unlink and replace a lease another process may have open. No process-wide
application mutex spans disk I/O or the lifetime of a Backtest.

Lease sidecars therefore remain as small tombstones after a Result is purged.
This intentional per-result metadata growth avoids an unlink/recreate race that
could split concurrent processes across different file identities; automatic
lease-file garbage collection is outside this repair.

Lifecycle mutations and recovery use the same per-ID ownership protocol. Valid
legacy terminal artifacts in Drafts/Results/Trash can obtain operational lease
metadata without changing canonical bytes. This exception does not apply to
legacy Staging. Busy mutations return a structured error and change nothing.

## 4. Retention before visibility

Initialization order is: validate configuration/Strategy, obtain a unique ID
and lease, acquire its distinct immutable segment references, revalidate exact
selection while pinned, then create Staging and return the writer. Invalid
configuration still creates no staged/draft/saved artifact.

The same pin-before-visibility rule applies to import: validate the source,
allocate a fresh Result ID and lease, acquire and revalidate its referenced
segments, then copy/rewrite the imported artifact in Staging and promote it.
Neither import nor recovery may expose a Draft/Result/Trash entry before its
references are held. Existing terminal file contents remain canonically
unchanged when only their location or operational lease metadata changes.

Reuse `SegmentRetentionStore::acquire`, which validates content hashes and
coordinates references with `BEGIN IMMEDIATE`. Keep pin ownership under the
same Result ID through Staging, Drafts, Results and Trash. Ordinary Save and
rename neither release nor change those references.

Recoverable files become visible only after their references are confirmed.
Recovery must not move a file and then attempt its first acquisition. A failed
pin or exact-selection check exposes no artifact and changes no existing run.
Undo newly acquired initialization pins only while the lease is held and after
proving this attempt never made an artifact visible. Crash-orphaned pin/lease
metadata is conservatively retained and reported, not automatically collected
from a guessed absence of ownership. Safe orphan reclamation is outside this
repair; bounded retention is preferable to deleting possibly referenced data.

Lock order is per-result lease, then a short retention transaction, then artifact
transactions. Do not hold a retention transaction while waiting for another
lease or processing an Engine bar. Cancellation/failure never unpins a visible
recoverable prefix. Existing explicit Discard/Purge remains the release seam.

## 5. Recorded Engine execution

Add a proposed `BacktestExecution::startRecorded(BacktestRequest,
const results::ResultStore &, const results::RunDescriptor &)` factory returning
the existing fallible unique execution owner. Validate Strategy and exact bars
before creating its writer. Recording mode is fixed at construction; callers
use the existing `advance` seam and cannot accidentally choose an unrecorded
step for a recorded execution. The existing `start` remains in-memory only.

The recorded owner holds its writer and committed-record count. Each `advance`
uses the same existing Engine transition, then persists only the new bar batch:
initial warnings where applicable, orders, fills, costs and the last post-bar
portfolio checkpoint, with unchanged sequence numbers and timestamps. Commit
the complete batch in one artifact transaction before exposing that bar's
progress or allowing the next bar to execute. Never rewrite the cumulative
timeline per bar or copy all preceding records to obtain the new batch.

The append must commit before `advance()` reports success or emits a progress
snapshot to Bindings/Frontend. If the append fails, the recorded execution
becomes permanently terminal. It must not expose the uncommitted bar's fills,
records or portfolio checkpoint, must not step the Engine again, and must not
retry that transition. Discard the uncommitted in-memory presentation delta and
report the last known committed checkpoint plus the storage error; the mutable
Engine state is no longer resumable. Release the artifact connection before
the lease. On the next store open, SQLite recovery and prefix validation
determine whether an ambiguous commit actually reached disk; application code
never retries an uncertain append and risks duplicate canonical events.

If persistence fails, stop this execution and expose the last committed
financial state with a storage error. No retry re-executes a bar, Strategy
observation, fill or order. Do not label the uncommitted bar processed or
present its trades. Release resources safely; recovery determines the actual
SQLite-committed prefix if the process died between commit and acknowledgment.

Add a proposed `finalizeRecorded()` seam for the owned writer. Both existing
`runBacktestAndRecord` overloads must share this recorded transition, including
the synchronous borrowed-writer compatibility overload whose borrow cannot
escape the call. Existing `finalizeAndRecord(writer)` remains supported and
must not append already committed records twice.

Terminal-only canonical records and terminal metadata/summary/hash must commit
atomically. Propose a four-argument `ResultWriter::finalizeDraft` overload taking
the terminal record tail, while retaining the existing three-argument seam.
Its transaction inserts the tail and finalizes together; a crash cannot leave
a Running container with a separately committed incomplete terminal tail.
Summary eligibility and fixed-point values remain unchanged.

The artifact remains WAL with full synchronization. SQLite's durability relies
on working filesystem/device synchronization; this design does not promise
protection from storage that lies about flushing.
[SQLite synchronous documentation](https://www.sqlite.org/pragma.html#pragma_synchronous)

## 6. Recovery and explicit publication

Only inspect a modern writer-Staging artifact after acquiring and validating its lease.
SQLite first resolves any unfinished transaction through its normal journal
protocol; do not copy, truncate or delete its WAL/journal sidecars manually.
[SQLite locking and recovery](https://www.sqlite.org/lockingv3.html)

If a Staging artifact has no lease file, treat it as legacy-unleased before
opening its SQLite payload: leave the file byte-identical in Staging, do not
attempt to claim it, and attach a persistent recovery notice. If a lease file is
busy, skip the artifact without reading it and do not report corruption. If a
lease is available but its metadata, artifact, data references or committed
prefix is invalid, preserve all files/references and report a notice; do not
quarantine or move the artifact automatically.

Apply the same rule to legacy `.discarding.bteresult` and `.purging.bteresult`
removal sidecars: without a lease, preserve and report them rather than guessing
whether the old removal had released pins. New discard/purge operations acquire
the lease before creating a removal sidecar and hold it through file removal and
the retention transaction. This preserves the old artifact/reference state on
every recoverable failure boundary.

A valid Running artifact has either no canonical records or a complete
chronological prefix ending in the last post-bar portfolio checkpoint. Validate
all records, exact data and pins. A zero-bar prefix becomes an empty Interrupted
Draft. A nonempty prefix preserves every complete recorded bar/fill/cost and
becomes an Interrupted Draft with no eligible final summary. Do not synthesize
future bars, fills, portfolio checkpoints or terminal liquidation.

Unexpected trailing records, invalid metadata/hash/schema or unavailable data
fail closed: preserve the artifact and its references, report the reason and
do not expose it as a trusted replay. Do not infer a complete bar from a fill
alone. Valid already-terminal staging preserves its status; reopening never
turns an Interrupted or Failed run into Completed.

Promotion validates, closes and uses the existing same-filesystem no-clobber
transition while holding ownership. Keep references intact if promotion or
catalog refresh fails; a later recovery must be idempotent. Drafts stay outside
Saved Results until explicit Save. Save/Save & Replay retains the same ID and
never reruns Engine. Recovery does not automatically publish.

## 7. Notices and module seam changes

Propose an immutable `results::ResultRecoveryNotice` value with Result ID,
typed reason and English diagnostic, plus a read-only
`ResultStore::recoveryNotices()` accessor. Notices describe artifacts preserved
for legacy-unleased staging, invalid lease/prefix/data or initialization-orphan
metadata. An expected busy live writer is skipped, not presented as corrupt.

Storage-root/catalog initialization errors still fail `ResultStore::open()`;
an individual preserved Staging or orphan-lease artifact must not prevent valid
Saved Results/Drafts from being listed. Cancellation still propagates. Preserve
the existing `ResultStore::list*()` signatures; Bindings bundle the selected
catalog entries and a copy of the immutable notices from that same opened store
into a value-owned `ResultCatalogSnapshot` for queued UI delivery.

For orphan detection, compare valid lease IDs against the four artifact
directories and query whether the retention database still has references for
that Result ID. A lease that is busy is an active initialization and is skipped.
An unlocked lease with retained references but no artifact produces an
`orphanedInitialization` notice and remains untouched. An unlocked, valid
tombstone with no references is expected after purge and produces no warning.
No orphan path automatically releases references or unlinks the lease file.

Because Core currently has no contention code, add
`core::ErrorCode::resourceBusy` so a mutation that cannot acquire a Result lease
can tell the caller to retry rather than mislabel contention as corruption or
an internal failure. Notices use their own typed reasons
(`legacyUnleased`, `invalidLease`, `invalidPrefix`, `missingReferences`, and
`orphanedInitialization`); English UI text comes from those notices, not from
parsing free-form error messages.

Both Result-library and Replay catalog callers display a non-destructive
English warning without hiding other valid results. Draft/catalog callers also
receive the notices when they list interrupted drafts. This changes typed
Bindings request result aliases and requires explicit tests at each caller; it
does not break the existing ResultStore list APIs.

Do not put SQLite types into public headers, let Frontend inspect Staging paths,
or introduce a second execution/accounting authority. A private concrete RAII
lease is shared by writer/recovery/lifecycle operations, not a hypothetical
polymorphic lock interface with one implementation.

## 8. Required evidence before acceptance

- Real two-process tests: a live writer remains untouched by another store open;
  termination releases ownership and one recovery retains exactly committed
  bars. Competing recoveries/mutations do not clobber or release each other's pins.
- Legacy unleased Staging, including empty and partially written files, stays
  byte-identical in place and produces a notice; no PID/time-based takeover.
- A crash before/after initialization pinning, each bar commit, terminal commit,
  close and no-clobber promotion never exposes an unpinned or partial bar.
- Import pinning failure leaves no visible imported Result; successful import
  retains every referenced segment before promotion. Recovery with a missing
  pin preserves Staging and reports a notice without releasing any reference.
- Failed append/commit stops before the next Strategy evaluation, keeps prior
  records/financial state, and creates no duplicate fills after recovery/Save.
- Zero-bar interruption, one-bar prefix, multiple Buy/Sell bars, cancellation,
  invalid tail, missing data, alias paths and repeated recovery are covered.
- Uninterrupted recorded Batch/Paced executions match literal canonical records
  and the unchanged schema-2 hash; pacing/leases/notices do not enter hashing.
- English notices are exercised through actual Results/Replay widgets, including
  a valid saved result beside preserved legacy Staging. No delete button is
  offered as automatic remediation.
- Measure recorded 23,777-bar execution and recovery without cumulative O(n²)
  work; include full tests, non-fast Quality, applicable ASan/TSan and all new
  process tests on the exact submission. Mechanical green is not final UX proof.

Use isolated temporary stores and explicitly owned child processes for crash
tests. No user result, market-data directory, existing stash or application is
deleted or terminated as a test fixture.

## 9. Implementation and acceptance status

The lease layout, additive Engine/Results seams, typed catalog notices and
conservative orphan policy above were approved before implementation. The
local implementation includes real child-process tests for live-writer
isolation, zero/one/multiple committed bars, alias paths, and recovery after
process death, plus Results/Engine/Bindings/UI fault and lifecycle tests.
R1–R3 code and local automated-test coverage are implemented and merged. The
exact PR #81 submitted head passed repository tests, Quality, ASan/TSan, and
required remote checks. Keep final native acceptance open until the actual
application flow is exercised against real saved data.
