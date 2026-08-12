# 0019 — Run full static analysis for every gate

- **Status**: Accepted
- **Date**: 2026-08-13
- **Deciders**: repository maintainer
- **Supersedes**: [ADR 0018](0018-run-full-static-analysis-on-manual-and-scheduled-ci.md)
- **Superseded by**: —

## Context

ADR 0018 kept pull-request and push analysis limited to changed translation
units while manual and scheduled workflows covered the entire project. That
left a pull request without direct evidence that unchanged translation units
still pass clang-tidy, cppcheck, and IWYU under the submitted toolchain. The
local quality command had the same limitation.

## Decision

Every CI event runs clang-tidy, cppcheck, IWYU, and scan-build for the complete
project compilation database. `RunQuality.sh` uses the same full-project
analyzer scope by default. Changed-line and changed-branch coverage remain
diff-based, because they measure the behavior introduced by the submitted
change rather than historical coverage debt.

The existing `static-analysis` and `merge-gate` dependencies remain required;
this change strengthens their scope without adding a dependency or bypass.

## Consequences

**Positive:**

- Every pull request receives whole-project static-analysis evidence.
- A single local quality command matches CI's analyzer scope.
- Toolchain or include-analysis regressions in unchanged units cannot wait for
  the weekly schedule.

**Negative:**

- Pull-request and push analysis can take longer as the project grows.

**Mitigation:**

- The job retains its 30-minute timeout and `RunQuality.sh --fast` still skips
  only scan-build, not the full clang-tidy, cppcheck, or IWYU checks.

## Alternatives considered

1. **Keep changed-unit PR analysis.** Rejected because it does not meet the
   required whole-project static-test evidence.
2. **Add a separate full-analysis job.** Rejected because it duplicates the
   configured build and creates two sources of analyzer truth.
3. **Run full analysis only weekly.** Rejected because a green PR could still
   lack whole-project evidence.

## References

- [ADR 0012](0012-enforce-static-analysis-and-diff-coverage.md)
- [ADR 0018](0018-run-full-static-analysis-on-manual-and-scheduled-ci.md)
- [`../Specs/10CiDevFlow.md`](../Specs/10CiDevFlow.md)
- [`../../RunQuality.sh`](../../RunQuality.sh)
