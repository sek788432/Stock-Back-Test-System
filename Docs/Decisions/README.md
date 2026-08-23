# Project Decisions

The current sources in this directory are:

- [`ImportantDecisions.md`](ImportantDecisions.md) — the single living record
  of important decisions that still constrain the project;
- [`Dependencies.md`](Dependencies.md) — the maintained dependency, version,
  license, and usage inventory.

The numbered ADR files are temporary migration sources for issue #53 phase 3.
The still-active portions of accepted numbered decisions remain authoritative
and required reading when relevant to the touched area, whether or not an
owning spec already links them, until each is atomically migrated with its
consumers. Do not add or amend a numbered ADR.
Phase 3 will migrate only
still-active rationale and important trade-offs into `ImportantDecisions.md`,
update every consumer, and delete the numbered archive. Git history retains
obsolete chronology.

## Recording an important decision

Before implementation, use the repository `grilling` workflow to resolve every
ambiguous product, compatibility, retention, or specification choice with the
maintainer. Add or update a living entry only when all three tests defined in
`ImportantDecisions.md` pass; an implemented merge-gate change always requires
an entry. Update the living entry and owning specification in the same reviewed
change. Explain in the PR why an entry is unnecessary when a listed category
does not pass the three tests.

Each living entry contains only:

- the current decision;
- why it was chosen;
- important rejected alternatives and why;
- consequences; and
- the owning specification.

The detailed threshold and maintenance rules live in
[`ImportantDecisions.md`](ImportantDecisions.md) and
[`../Governance/AGENTS.md`](../Governance/AGENTS.md) §5.

## Temporary phase-3 migration index

Use this complete index to discover still-active rationale until phase 3 maps
it into the living document and owning specs. Superseded content is migration
input only; accepted portions remain authoritative.

| # | Decision | Transition status |
|---|---|---|
| 0001 | [Record architecture decisions](0001-record-architecture-decisions.md) | Replaced by the phase-2 living-document policy; retained only as migration context |
| 0002 | [C++ and Qt desktop stack](0002-cpp-and-qt-as-the-desktop-stack.md) | Accepted; active portions remain authoritative |
| 0003 | [Hybrid rule and Lua authoring](0003-hybrid-rule-and-lua-strategy-authoring.md) | Superseded by 0011 |
| 0004 | [Semantic anti-cheat target](0004-anti-cheat-ci-gate-with-mutation-testing.md) | Accepted planned target; active rationale remains authoritative |
| 0005 | [Distribution and launcher decisions](0005-build-distribution-launcher-decisions.md) | Superseded by 0011 |
| 0006 | [Full-tree project standards](0006-full-tree-project-standards-gate.md) | Accepted; active rationale remains authoritative |
| 0007 | [Vendor shared agent skills](0007-vendor-shared-agent-skills.md) | Superseded by 0008 |
| 0008 | [Standardize project agent skills](0008-standardize-project-agent-skills.md) | Superseded by 0009 |
| 0009 | [Host adapters and centralized rules](0009-retain-host-adapters-and-centralize-hard-rules.md) | Accepted; active rationale remains authoritative |
| 0010 | [PascalCase paths and unit-test layout](0010-enforce-pascal-case-paths-and-unit-test-layout.md) | Accepted; active rationale remains authoritative |
| 0011 | [Engine and release-data contract](0011-own-the-engine-and-release-data-contract.md) | Accepted; active rationale remains authoritative |
| 0012 | [Static analysis and diff coverage](0012-enforce-static-analysis-and-diff-coverage.md) | Accepted portions remain authoritative; scope/thresholds partly replaced by 0017 and 0019 |
| 0013 | [CI supply-chain hardening](0013-harden-ci-supply-chain.md) | Accepted; active rationale remains authoritative |
| 0014 | [Starter backtest seam](0014-introduce-the-starter-backtest-seam.md) | Accepted; active rationale remains authoritative |
| 0015 | [Local quality and coverage reports](0015-run-quality-locally-and-publish-per-commit-coverage.md) | Accepted; active rationale remains authoritative |
| 0016 | [Selectable-condition strategy seam](0016-introduce-selectable-condition-strategy.md) | Accepted; active rationale remains authoritative |
| 0017 | [Coverage gates and artifact runtime](0017-raise-changed-code-coverage-gates-and-update-artifact-runtime.md) | Accepted; active rationale remains authoritative |
| 0018 | [Manual and scheduled full analysis](0018-run-full-static-analysis-on-manual-and-scheduled-ci.md) | Superseded by 0019 |
| 0019 | [Full analysis for every gate](0019-run-full-static-analysis-for-every-gate.md) | Accepted; active rationale remains authoritative |
| 0020 | [Expanded C++ standards enforcement](0020-expand-cpp-skill-standards-enforcement.md) | Accepted; active rationale remains authoritative |
