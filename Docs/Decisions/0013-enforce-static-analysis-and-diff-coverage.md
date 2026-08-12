# 0013 — Enforce static analysis and diff coverage

- **Status**: Accepted
- **Date**: 2026-08-12
- **Deciders**: repository maintainer
- **Supersedes**: —
- **Superseded by**: —

## Context

The merge gate builds and tests the project with warnings as errors and applies
the repository's line-oriented standards checker. It does not use compiler-AST
or data-flow analysis, check include ownership, or measure whether changed
executable lines and branches ran under tests. Those checks are already accepted
as target state in Specs 10 and ADR 0004, but commands and CI jobs did not exist.

Analyzer output must be reproducible locally, useful when CI fails, and narrow
enough to avoid scanning fetched dependencies as project code. Coverage must
judge the submitted change rather than impose a whole-tree threshold that would
hide or redistribute existing test debt.

## Decision

Add two required Ubuntu 24.04 workflow jobs and make `merge-gate` depend on both:

1. `static-analysis` configures the complete Qt source tree with Clang 18 and a
   compilation database, then runs clang-tidy 18, cppcheck 2.13, IWYU, and the
   Clang 18 static analyzer. Analyzer findings are errors. scan-build reports
   are uploaded even when analysis fails.
2. `coverage` builds and runs every registered backend and Qt test with GCC
   coverage instrumentation. gcovr 8.5 produces machine-readable and HTML
   reports. diff-cover 10.0.0 requires at least 90% changed-line coverage, and a
   repository-owned checker requires at least 80% changed-branch coverage.

Only translation units under `Src/` are analyzer roots; included project headers
remain visible to the tools, while fetched and system dependencies do not become
project findings. Coverage is filtered to `Src/`. A change with no executable
lines or branch sites has no applicable denominator and passes that metric.

The workflow runs for pull requests, pushes to `main`, a weekly schedule, and
manual dispatch. Ubuntu and Python tool package versions are pinned exactly and
printed in CI.

cppcheck is GPL-3.0-or-later, but it is executed only as a developer/CI program. It is not
linked into, packaged with, or redistributed as part of the Apache-2.0 product.
This ADR is the explicit tooling-only license decision required by governance.

## Consequences

**Positive:**

- Merge approval now includes complementary AST, data-flow, include, and
  changed-code test evidence.
- Local and CI commands share the checked-in configurations.
- HTML scan-build and coverage reports remain available for diagnosis.
- Existing whole-tree coverage debt does not let new untested behavior land.

**Negative:**

- Pull requests install more tooling and run two additional builds.
- Broad clang-tidy and IWYU policies may require focused corrections when a
  previously uncompiled source becomes part of the analysis preset.
- A removed Ubuntu package revision requires an explicit reviewed version bump.

**Mitigations:**

- Quality jobs run in parallel with the existing platform test matrix.
- The workflow has explicit timeouts and uploads diagnostic artifacts.
- Suppressions remain empty by default and require a narrow, reviewed reason.

## Alternatives considered

1. **Advisory-only analyzers.** Rejected because accepted G7 policy requires
   findings to affect merge status.
2. **Whole-project coverage threshold.** Rejected by ADR 0004 because it
   measures historical debt rather than the submitted change.
3. **Hosted coverage service.** Rejected because it adds credentials, external
   availability, and another policy surface without improving the gate.
4. **Run scan-build only weekly.** Rejected for this initial rollout so every
   submitted gate change receives the same data-flow analysis before merge; the
   weekly trigger remains useful for detecting toolchain drift.

## References

- [ADR 0004](0004-anti-cheat-ci-gate-with-mutation-testing.md)
- [`../Specs/10CiDevFlow.md`](../Specs/10CiDevFlow.md)
- [`../../.github/workflows/ci.yml`](../../.github/workflows/ci.yml)
