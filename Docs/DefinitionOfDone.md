# Definition of Done

A change is done only when every applicable item below is true. Copy the
applicable sections into the pull request. Write `N/A — <reason>` for an item
that does not apply; never claim that an unrun or nonexistent check passed.

## Universal — every change

- [ ] **Authorized scope:** the change addresses the requested concern and
  preserves unrelated and user-owned work.
- [ ] **Context:** the relevant specs, active important decisions, project
  skills, and [`Governance/AGENTS.md`](Governance/AGENTS.md) were read.
- [ ] **Status truth:** documentation identifies behavior as Implemented,
  Planned, or Blocked and does not describe planned tooling as enforced.
- [ ] **Self-review:** the complete diff was reviewed hunk by hunk.
- [ ] **Repository hygiene:** no secrets, dead commented-out code, generated
  build output, or task comments without an `ISSUE-NNN` reference were added.
- [ ] **Traceability:** the PR references its issue and any affected entry in
  the living important-decisions document, or explains why an entry is
  unnecessary.
- [ ] **History:** the branch and commit messages follow the repository's PR
  conventions when a branch or commit is part of the task.

## Documentation-only changes

- [ ] Relative links and referenced anchors resolve.
- [ ] Code/configuration examples are syntactically plausible and parse when a
  parser is available.
- [ ] Related indexes and canonical cross-references are updated.
- [ ] The change introduces no contradiction with a more authoritative spec or
  active important decision; any intentional decision change updates both the
  living entry and its owning spec.
- [ ] Build, runtime, and code-test sections are marked `N/A — docs-only` rather
  than falsely checked.

## Production behavior changes

- [ ] The affected target builds cleanly with no new compiler warnings.
- [ ] Every affected **public behavior** has unit tests for:
  - [ ] a positive case;
  - [ ] a negative case, including the specified error and absence of forbidden
    side effects;
  - [ ] meaningful boundary cases at and immediately around the limit.
- [ ] If a public behavior has no meaningful negative or boundary case, the PR
  gives a concrete reason; this is not a blanket exemption.
- [ ] Every bug fix has a focused regression test that fails under the old
  implementation.
- [ ] Every intentional change to public behavior has a focused regression test
  that captures the old-versus-new contract.
- [ ] Tests exercise real production code, contain meaningful assertions, and
  do not use trivial, tautological, empty, silently disabled, or mock-only
  substitutes for the unit under test.
- [ ] Existing registered tests pass.

Public behavior includes observable contracts exposed by public C++ or Python
APIs, UI actions, command interfaces, persisted formats, and generated
artifacts. Private helpers need direct unit tests only when that is the clearest
way to prove a public behavior.

## Cross-module and persistence changes

- [ ] IPC protocols, immutable snapshot generation, and `.bteresult` schema or
  lifecycle changes have contract/integration tests in addition to unit tests.
- [ ] Failure, interruption, invalid-schema, and version-mismatch paths are
  covered where applicable.
- [ ] Authoritative financial values remain fixed-point at storage and engine
  boundaries.
- [ ] Identical immutable inputs produce identical canonical functional records
  and `canonicalResultHash`; physical SQLite bytes, local paths, and wall-clock
  metadata are not compared.
- [ ] An intentional semantic change updates its regression expectation and,
  once implemented, the canonical determinism fixture with an explanation.

## Threading and Python-worker changes

- [ ] Cross-thread state is immutable or owned by exactly one thread.
- [ ] Resources and process lifetime use RAII; no detached threads or manual
  mutex `lock()`/`unlock()` were introduced.
- [ ] Qt widgets are touched only on the UI thread through queued delivery.
- [ ] Cancellation, timeout, worker failure, protocol violation, and cleanup
  boundaries have tests when affected.

## Performance-sensitive changes

- [ ] The PR identifies the affected hot path and includes before/after evidence
  using a repository benchmark if one exists, or a documented reproducible
  measurement otherwise.
- [ ] Allocation, copying, algorithmic complexity, and synchronization changes
  were reviewed.
- [ ] Functional determinism is preserved or the intentional change is
  explained and regression-tested.

## CI and tooling changes

- [ ] The tool or workflow exists in the submitted tree and its documented
  command matches the implementation.
- [ ] Tool behavior has positive, negative, and boundary tests where applicable.
- [ ] `python3 Tools/CheckProjectStandards.py --full-tree --clang-format --base
  <base> --head <head>` passes for committed revisions.
- [ ] The complete workflow was run on a real branch when changing an
  implemented merge gate.
- [ ] Newly proposed but unimplemented checks remain labeled **Planned / not
  merge-blocking**.

## Before requesting CI for C++ changes

- [ ] `./RunTest.sh` passes on the exact committed `HEAD` being submitted.
- [ ] `./RunQuality.sh --base <base-revision> --head HEAD` passes on that same
  commit. `--fast` is for iteration and is not sufficient for this item.
- [ ] Generated coverage reports remain under `Output/` and are not committed.

## Verified commands currently available

Use the applicable commands from the checked-out repository:

```bash
cmake --preset qt-dev -DBTE_BUILD_TESTS=ON -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
cmake --build --preset qt-dev --parallel
ctest --preset qt-dev --no-tests=error
```

Both merge-blocking sanitizer configurations are available locally:

```bash
cmake --preset dev-sanitize
cmake --build --preset dev-sanitize --parallel
ctest --preset dev-sanitize --no-tests=error

cmake --preset dev-tsan
cmake --build --preset dev-tsan --parallel
ctest --preset dev-tsan --no-tests=error
```

Sanitizers are merge-blocking workflow jobs. See
[`Specs/CiDevFlow.md`](Specs/CiDevFlow.md) for the exact implemented
and planned enforcement status.

Static analysis and changed-code coverage are merge-blocking. Their exact local
commands and required tool versions are documented in
[`Specs/CiDevFlow.md`](Specs/CiDevFlow.md) §7.

For the complete pre-CI C++ quality workflow, run:

```bash
./RunTest.sh
./RunQuality.sh --base origin/main --head HEAD
```

## Release changes

- [ ] Version and user-visible release metadata are consistent.
- [ ] [`Governance/CHANGELOG.md`](Governance/CHANGELOG.md) is updated.
- [ ] Packaging and installation were verified on every declared release
  platform, or an unsupported platform is clearly excluded.
- [ ] Third-party package versions, hashes, licenses, and the release SBOM are
  recorded.
- [ ] Market-data redistribution rights and the verified split manifest are
  present before a public release containing project-managed data.

## When an item cannot be completed

Narrow or split the work, record a genuine blocker, or ask for help. Do not
weaken a requirement, disable a merge gate, or describe planned work as done.
