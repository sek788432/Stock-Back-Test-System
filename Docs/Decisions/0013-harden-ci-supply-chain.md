# 0013 — Harden the CI supply chain

- **Status**: Accepted
- **Date**: 2026-08-12
- **Deciders**: repository maintainer
- **Supersedes**: —
- **Superseded by**: —

## Context

The implemented workflow limits its token to read-only repository contents and
does not expose repository secrets to pull-request jobs. However, external
actions were referenced through mutable major-version labels, checkout retained
the workflow token in Git configuration, and pip resolved unhashed transitive
coverage dependencies at job runtime. Those choices leave avoidable
supply-chain and credential-exposure paths even on ephemeral hosted runners.

## Decision

Pin every external action reference to a full 40-character commit SHA and keep
the corresponding release label in a comment for review and automated update
metadata. Configure every checkout with `persist-credentials: false` because no
later step needs authenticated Git access.

Install gcovr, diff-cover, and their complete transitive dependency closure
from `Tools/CoverageRequirements.txt` with exact versions, artifact hashes, and
binary-only resolution. Keep the direct requirements in
`Tools/CoverageRequirements.in`; regenerate the lock with pip-tools 7.5.2 on
Python 3.12. CI prints the installed gcovr and diff-cover versions explicitly.

Focused repository tests reject mutable external action references, checkout
steps that persist credentials, and coverage installation without the locked
hash policy.

## Consequences

**Positive:**

- Upstream action labels cannot silently change code executed by CI.
- Pull-request-controlled build and test processes cannot recover a checkout
  token from Git's persisted credentials.
- Coverage dependency artifacts are authenticated against reviewed hashes and
  do not drift through unconstrained transitive resolution.

**Negative:**

- Action and Python dependency upgrades require an explicit lock or SHA update.
- The generated lock is larger because it records hashes for supported wheels.
- The coverage dependency lock currently targets Python 3.12, matching Ubuntu
  24.04 CI; a runner Python upgrade requires regeneration and review.

## Alternatives considered

1. **Keep major-version action labels.** Rejected because labels are mutable.
2. **Rely only on read-only token permissions.** Rejected because an
   unnecessary token remains useful for metadata access and should not be
   exposed to untrusted build code.
3. **Pin only direct Python packages.** Rejected because transitive packages
   and downloaded artifacts could still change without review.
4. **Add deployment credentials or privileged runners.** Out of scope; this
   hardening does not authorize secrets or privileged execution in PR jobs.

## References

- [GitHub Actions secure-use guidance](https://docs.github.com/en/actions/reference/security/secure-use)
- [ADR 0012](0012-enforce-static-analysis-and-diff-coverage.md)
- [`../Specs/10CiDevFlow.md`](../Specs/10CiDevFlow.md)
- [`../../.github/workflows/ci.yml`](../../.github/workflows/ci.yml)
