# 0018 — Run full static analysis on manual and scheduled CI

- **Status**: Accepted
- **Date**: 2026-08-12
- **Deciders**: repository maintainer
- **Supersedes**: The static-analysis scope in 0012
- **Superseded by**: —

## Context

ADR 0012 limits clang-tidy, cppcheck, and IWYU to translation units changed
between the event base and submitted commit. That is the right merge-gate scope
for pull requests and pushes: it gives fast, relevant feedback while scan-build
still analyzes the complete build.

Manual dispatch and scheduled runs can contain no changed C++ translation unit.
They then complete the three analyzers successfully without invoking them,
which makes an operator-requested static-analysis run appear to have skipped its
checks. The observed run at `6ae55a2` had this exact shape: it changed CI and
documentation files only.

## Decision

Keep changed-translation-unit analysis for `pull_request` and `push` events.
For `workflow_dispatch` and `schedule`, omit `--base` and `--head` when calling
`Tools/RunStaticAnalysis.py`; its default scope is every project translation
unit in the generated compilation database. `scan-build` continues to analyze
the complete build on every event.

All analyzer findings remain merge-blocking through the existing
`static-analysis` and `merge-gate` jobs. This change expands validation for
operator-requested and periodic runs; it does not weaken any existing gate.

## Consequences

**Positive:**

- Manual CI dispatch always executes clang-tidy, cppcheck, and IWYU against the
  project, including after docs-only or tooling-only commits.
- The weekly scheduled run detects static-analysis debt and toolchain drift
  across the full project.
- Pull-request and push latency keeps the focused changed-code scope from ADR
  0012.

**Negative:**

- Manual and scheduled static-analysis jobs may take longer as the project
  grows.

**Mitigation:**

- The workflow retains its explicit 30-minute timeout, and the already-complete
  scan-build pass provides an upper-bound signal for complete-tree cost.

## Alternatives considered

1. **Leave manual dispatch incremental.** Rejected because a successful
   docs-only dispatch produces no clang-tidy, cppcheck, or IWYU evidence.
2. **Analyze the entire project for every pull request and push.** Rejected
   because it adds routine latency without improving the changed-code gate.
3. **Remove the changed-file filter.** Rejected because it would discard useful
   scope control for routine merge validation.

## References

- [ADR 0012](0012-enforce-static-analysis-and-diff-coverage.md)
- [`../Specs/10CiDevFlow.md`](../Specs/10CiDevFlow.md)
- [`../../Tools/RunStaticAnalysis.py`](../../Tools/RunStaticAnalysis.py)
