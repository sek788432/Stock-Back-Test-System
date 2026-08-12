# 0015 — Run quality locally and publish per-commit coverage

- **Status**: Accepted
- **Date**: 2026-08-12
- **Deciders**: repository maintainer
- **Supersedes**: —
- **Superseded by**: —

## Context

The static-analysis and changed-code coverage gates from ADR 0012 have exact
local commands, but developers and coding agents must assemble those commands
manually. That makes it too easy to discover analyzer or coverage failures only
after pushing and waiting for CI.

Coverage HTML is currently retained as a workflow artifact. GitHub requires the
artifact to be downloaded before it can be viewed. Committing generated reports
would make the repository carry stale, high-conflict build output and would not
prove that the report was produced from the commit that contains it.

## Decision

Add a repository-owned local quality entry point that runs the same configured
static analyzers and coverage thresholds as CI. The canonical agent playbook and
contributor documentation require the complete registered tests and this local
quality workflow to pass before a branch is pushed or CI is requested. The local
command accepts explicit base and head revisions so its evidence is tied to the
commits being submitted. Its first invocation installs missing platform analyzer
packages and creates a private hash-locked coverage environment under `Output/`;
later invocations reuse that environment without shell activation or manual path
configuration.

CI remains the authority that regenerates coverage from each submitted commit.
It continues to reject changed C++ code below 90 percent changed-line coverage
or 80 percent changed-branch coverage. CI also writes the generated gcovr
Markdown summary and changed-code gate results to the GitHub Actions job summary
so they can be viewed in the browser without downloading the HTML artifact.

Generated coverage XML, JSON, Markdown, and HTML remain untracked build output.
The HTML artifact remains available for detailed diagnosis. GitHub's native Code
Quality coverage upload or a GitHub Pages deployment may be added separately
after the repository plan, settings, permission scope, fork policy, retention,
and pinned action revisions are verified.

## Consequences

**Positive:**

- Developers and agents get one repeatable command before spending CI time.
- The checked coverage thresholds remain identical locally and in CI.
- Every CI report is regenerated from, and attributable to, the submitted SHA.
- Coverage totals and changed-code results are visible on the workflow page.
- Generated reports do not create repository churn or merge conflicts.

**Negative:**

- First-run local quality execution requires network access and a supported
  platform package manager; Ubuntu may request the user's `sudo` password.
- The complete local workflow is slower than an ordinary incremental build.
- The full annotated-source HTML report remains an artifact until a safe
  publication mechanism is configured.

## Alternatives considered

1. **Commit generated coverage reports.** Rejected because reports can be stale,
   are easy to modify independently of tests, create frequent conflicts, and
   violate the repository rule against generated build output.
2. **Rely only on CI.** Rejected because feedback arrives after a push and wastes
   developer and hosted-runner time.
3. **Install a hosted coverage service immediately.** Rejected consistently with
   ADR 0012; the existing gate does not need another external availability or
   policy dependency.
4. **Publish every pull request's generated HTML through GitHub Pages.** Deferred
   because untrusted pull-request content must not receive deployment permission,
   and repository settings and retention policy are external to this checkout.

## References

- [ADR 0012](0012-enforce-static-analysis-and-diff-coverage.md)
- [ADR 0013](0013-harden-ci-supply-chain.md)
- [`../Specs/10CiDevFlow.md`](../Specs/10CiDevFlow.md)
- [`../../.github/workflows/ci.yml`](../../.github/workflows/ci.yml)
