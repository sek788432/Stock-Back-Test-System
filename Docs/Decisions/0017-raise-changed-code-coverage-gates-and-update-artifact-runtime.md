# 0017 — Raise changed-code coverage gates and update artifact runtime

- **Status**: Accepted
- **Date**: 2026-08-12
- **Deciders**: repository maintainer
- **Supersedes**: coverage-threshold portions of [ADR 0012](0012-enforce-static-analysis-and-diff-coverage.md) and [ADR 0015](0015-run-quality-locally-and-publish-per-commit-coverage.md)
- **Superseded by**: —

## Context

The implemented pull-request coverage gates require 90 percent changed-line
coverage and 80 percent changed-branch coverage. The repository maintainer
requires stronger evidence for newly changed C++ behavior: 98 percent changed
lines and 90 percent changed branches.

GitHub Actions warns that the pinned `actions/upload-artifact` v4 action targets
the deprecated Node.js 20 runtime. GitHub-hosted runners currently force it to
Node.js 24, producing warnings in every report-uploading job.

## Decision

- Raise the merge-blocking changed-code thresholds to 98 percent line coverage
  and 90 percent branch coverage.
- Make `RunQuality.sh`, `Tools/RunCoverageGates.sh`, and GitHub Actions enforce
  the same values.
- Upgrade every `actions/upload-artifact` workflow use to the full immutable
  SHA for v6.0.0, which natively runs on Node.js 24.

## Consequences

**Positive:**

- New pull-request code needs near-complete execution evidence before merge.
- Local quality results remain equivalent to CI thresholds.
- Artifact uploads no longer rely on GitHub forcing a deprecated action runtime.

**Negative:**

- Contributors may need more focused tests for exceptional and boundary paths.
- `actions/upload-artifact` v6 requires self-hosted runners to be at least
  version 2.327.1; this repository uses GitHub-hosted runners.

## Alternatives considered

1. **Keep the existing 90/80 thresholds.** Rejected because they do not meet
   the requested pull-request quality bar.
2. **Raise only CI thresholds.** Rejected because local evidence would no
   longer predict merge eligibility.
3. **Set an environment override to keep Node.js 20.** Rejected because it
   preserves a deprecated runtime instead of upgrading the pinned action.

## References

- [ADR 0012](0012-enforce-static-analysis-and-diff-coverage.md)
- [ADR 0015](0015-run-quality-locally-and-publish-per-commit-coverage.md)
- [`../Specs/10CiDevFlow.md`](../Specs/10CiDevFlow.md)
- [upload-artifact v6.0.0 release](https://github.com/actions/upload-artifact/releases/tag/v6.0.0)
