# 10 — CI and Development Flow

## 1. Status and authority

This spec separates policy from mechanical enforcement:

- **Implemented / merge-blocking:** present in `.github/workflows/ci.yml` and
  required by its aggregate `merge-gate` job.
- **Implemented / local:** present in the repository but not required by the
  workflow aggregate.
- **Planned / not merge-blocking:** required target state whose tool or workflow
  does not yet exist.
- **External / unverified:** a GitHub repository setting that cannot be proven
  from the checked-out files.

A requirement does not become merge-blocking merely because this spec describes
it. It becomes merge-blocking only when the submitted workflow runs it and the
aggregate gate depends on it.

## 2. Implemented merge-blocking workflow

The repository currently has one workflow:
[`../../.github/workflows/ci.yml`](../../.github/workflows/ci.yml). It runs on
pull requests, pushes to `main`, a weekly schedule, and manual dispatch.

| Workflow job | Verified behavior | Status |
| --- | --- | --- |
| `all-tests` | Installs Qt 6.9 with Qt Charts; configures and builds the `qt-dev` preset with tests and compiler warnings as errors; runs every registered CTest test on Ubuntu and macOS. | Implemented / merge-blocking |
| `project-standards` | Runs `Tools/CheckProjectStandards.py --full-tree` against the submitted Git revision on Ubuntu. | Implemented / merge-blocking |
| `static-analysis` | Configures the complete Qt source tree with Clang 18; runs clang-tidy, cppcheck, and IWYU on changed translation units; and runs scan-build on the complete build. Analyzer findings fail the job; scan-build reports are uploaded. | Implemented / merge-blocking |
| `coverage` | Builds and runs all registered tests with coverage instrumentation; requires 90% changed-line coverage and 80% changed-branch coverage; uploads gcovr reports. | Implemented / merge-blocking |
| `merge-gate` | Fails unless both matrix test runs, project standards, static analysis, and changed-code coverage succeed. Its display name is `Merge gate (all required checks)`. | Implemented / merge-blocking |

The workflow does **not** currently run Windows, DataFetcher pytest, mutation
testing, ruff, explicit format checks, sanitizers, TSan, or a determinism
fixture comparison.

Every external action is pinned to a full commit SHA, checkout credentials are
not persisted into the repository configuration, and coverage dependencies are
installed from the hash-locked `Tools/CoverageRequirements.txt`. Release labels
remain beside action SHAs as review and dependency-update metadata; labels are
not executable references.

Whether GitHub branch protection requires the aggregate status, signed commits,
reviews, or linear history is **External / unverified**. Those settings must be
checked in GitHub; repository files alone cannot prove them.

## 3. Implemented project-standards check

[`../../Tools/CheckProjectStandards.py`](../../Tools/CheckProjectStandards.py)
is a line-oriented repository checker. In full-tree mode it inspects tracked
UTF-8 files from the exact `--head` commit and skips binary files.

It currently checks:

- trailing whitespace;
- selected banned C++ spellings, including `using namespace std`, raw
  allocation/deallocation, selected C string/output functions, manual
  `lock()`/`unlock()`, and detached threads;
- task comments in source/configuration that lack `ISSUE-NNN`;
- literal/identical assertion patterns that its regular expressions recognize;
- registration of tracked `UnitTest_*.cpp` and `UnitTest_*.py` files in CMake;
- a registered unit-test file for each newly introduced top-level source
  module.

It does not parse C++ or Python semantics, prove public-behavior coverage, find
every empty or mock-only test, or replace compilation and runtime tests. Its own
Python unit tests are registered in CTest as `bte_project_standards_tests`.

For two committed revisions, run:

```bash
python3 Tools/CheckProjectStandards.py \
  --full-tree --base <base-revision> --head <head-revision>
```

The `--full-tree` form requires `--head`; it does not audit uncommitted files.
Use ordinary diff review for the working tree, then run the command against the
resulting commit.

## 4. Required testing contract

Every affected public behavior requires unit tests in all three categories:

- **Positive:** valid input produces the specified result and side effects.
- **Negative:** invalid input or an unavailable operation produces the specified
  error and no forbidden side effect.
- **Boundary:** values at and immediately around each meaningful limit produce
  the specified result.

Public behavior means an observable contract exposed through a public C++ or
Python API, UI action, command interface, persisted format, or generated
artifact. It does not mean that every private helper needs a separate test.
One focused test may cover multiple categories if its assertions prove each
category. If a behavior has no meaningful negative or boundary case, the PR
must state the concrete reason.

In addition:

- Every bug fix requires a regression test that fails under the previous
  implementation and passes with the fix.
- Every intentional public-behavior change requires a regression test that
  captures the old-versus-new contract.
- IPC, immutable snapshot generation, and `.bteresult` persistence require
  contract/integration tests in addition to unit tests.
- Tests must exercise production behavior and contain meaningful assertions.
  Literal truths, tautologies, empty tests, silently disabled tests, and tests
  that only prove a mock's configured behavior are invalid even when the
  current checker does not detect them.

These are project requirements today. Full mechanical parity enforcement is
Planned / not merge-blocking until the corresponding tools exist.

## 5. Functional determinism contract

Identical immutable inputs must produce identical canonical functional records
and the same `canonicalResultHash`.

Input identity includes at least the engine version, runtime profile, strategy
artifact and API version, normalized run configuration, numeric/rounding policy,
and immutable market-snapshot identifiers and hashes.

Canonical functional records include orders, cancellations, fills, trades,
positions, portfolio/equity state, P&L, fees, strategy-relevant indicator
snapshots, margin and corporate-action events, warnings, and strategy logs.
Their canonical serialization must define field encoding and record order rather
than depend on container iteration order.

The following are non-functional and excluded from canonical comparison:

- SQLite page layout and transaction metadata;
- the raw bytes of a `.bteresult` container;
- local paths and installation directories;
- wall-clock creation time and UI state.
- Batch/Paced Backtest delays and K-line Replay presentation state.

An intentional semantic change must include a regression test and explain the
new canonical expectation. The automated canonical fixture comparison is
Planned / not merge-blocking; until it exists, reviewers verify this contract
through focused tests and result inspection.

Batch and Paced Backtest execution must compare equal under this contract.
K-line Replay is verified separately to read and present persisted records
without invoking Strategy hooks, Python, indicator scheduling, order evaluation,
or fill creation.

## 6. Planned checks — not yet enforced

The following work is explicitly unimplemented:

- Windows build and registered CTest execution.
- DataFetcher pytest execution.
- Semantic anti-cheat analysis.
- Public-behavior parity, including positive/negative/boundary parity.
- Mutation testing.
- clang-format and ruff-format enforcement.
- ruff analysis.
- ASan, UBSan, LSan, and TSan workflow jobs.
- Canonical functional-result fixture comparison.
- Repository-verifiable reviewer and branch-protection policy checks.

Implement planned checks incrementally. Each implementation must include tests
for the checker, update this status table in the same PR, and make the aggregate
job depend on the new job before the check may be called merge-blocking.

No threshold, exemption file, report artifact, or tool command should be
documented as current until that exact mechanism exists in the tree.

## 7. Local verification

The local commands matching the implemented C++/Qt workflow are:

```bash
cmake --preset qt-dev -DBTE_BUILD_TESTS=ON -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
cmake --build --preset qt-dev --parallel
ctest --preset qt-dev --no-tests=error
```

For applicable C++ changes, a sanitizer preset is available locally:

```bash
cmake --preset dev-sanitize
cmake --build --preset dev-sanitize --parallel
ctest --preset dev-sanitize --no-tests=error
```

The sanitizer preset is Implemented / local, not merge-blocking. No repository
pre-commit configuration or changed-test runner currently exists.

The merge-blocking analyzer commands are:

```bash
CC=clang-18 CXX=clang++-18 cmake --preset analysis -DBTE_BUILD_QT_APP=ON
cmake --build --preset analysis --parallel
python3 Tools/RunStaticAnalysis.py clang-tidy --base <base> --head <head>
python3 Tools/RunStaticAnalysis.py cppcheck --base <base> --head <head>
python3 Tools/RunStaticAnalysis.py iwyu --base <base> --head <head>
scan-build-18 --use-analyzer=/usr/bin/clang-18 cmake -S . -B Output/scan-build -DBTE_BUILD_TESTS=OFF -DBTE_BUILD_QT_APP=ON
scan-build-18 --use-analyzer=/usr/bin/clang-18 --status-bugs --keep-empty -o Output/scan-build-reports cmake --build Output/scan-build --parallel
```

The analysis build materializes Qt-generated translation units referenced by
the compilation database before cppcheck loads that database.

CI pins clang/clang-tidy/clang-tools `1:18.1.3-1ubuntu1`, cppcheck
`2.13.0-2ubuntu3`, and IWYU `8.21-1build2` on Ubuntu 24.04. The complete Python
coverage dependency closure—including gcovr `8.5`, diff-cover `10.0.0`, Jinja2
`3.1.6`, and MarkupSafe `3.0.3`—and artifact hashes are recorded in
`Tools/CoverageRequirements.txt`; direct requirements remain in
`Tools/CoverageRequirements.in`.

The merge-blocking coverage commands are:

```bash
cmake -E remove_directory Output/coverage
cmake --preset coverage -DBTE_BUILD_QT_APP=ON
cmake --build --preset coverage --parallel
ctest --preset coverage --no-tests=error
python3 -m gcovr --root . --filter 'Src/' --merge-mode-functions merge-use-line-min --decisions --cobertura coverage.xml --json coverage.json --html-details coverage.html
diff-cover coverage.xml --compare-branch=<base> --fail-under=90
python3 Tools/CheckDiffBranchCoverage.py coverage.json --base <base> --head <head> --fail-under 80
```

The branch gate uses gcovr's `--decisions` records, so its denominator is
source-level C++ conditional and switch outcomes on changed lines. Raw GCC
control-flow edges are not used because they include compiler-generated
exception and cleanup paths that do not correspond to testable source branches.

## 8. Gate changes and failures

- Never disable or bypass an implemented merge gate to land a change.
- Changing a CI gate requires an ADR, focused tooling tests, and review.
- A failing required test is fixed in production or in the test; it is not
  commented out or silently skipped.
- A temporary skip must reference an open issue and remain visible to reviewers;
  the current checker does not validate issue existence.
- There is no generic undocumented override for `all-tests`,
  `project-standards`, or `merge-gate`.

## 9. Test layout

C++ and Qt tests live under `Tests/` and are registered from
[`../../Tests/CMakeLists.txt`](../../Tests/CMakeLists.txt). Existing source and
test layout should be mirrored when adding a module. Test filenames use
`UnitTest_<Behavior>.cpp` or `UnitTest_<Behavior>.py` so the current registration
audit can find them.

DataFetcher currently has no registered pytest suite. When one is implemented,
its location, discovery command, dependencies, and merge status must be added
here rather than assumed.
