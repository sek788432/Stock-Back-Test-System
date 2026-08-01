# 0008 — Standardize project agent skills

- **Status**: Accepted
- **Date**: 2026-08-01
- **Deciders**: project maintainers
- **Supersedes**: 0007
- **Superseded by**: —

## Context

The repository stored its five C++ workflow skills under `.cursor/skills` and
the vendored Matt Pocock collection under `.agents/skills`. It also carried a
host-specific root instruction file. That layout duplicated concepts, made the
source of truth unclear, and coupled project guidance to particular agent
products.

The open Agent Skills format makes each skill portable, but agent products do
not all discover the same filesystem locations. The project prefers one clean,
host-neutral source over native discovery in every current tool.

## Decision

Use `.agents/skills` as the repository's only project-skill directory.

- Move the five repository-specific C++ skills into `.agents/skills` beside
  the vendored engineering and productivity skills.
- Put all future skills, including `stockbt-actions`, under `.agents/skills`.
- Remove `.cursor`, `.claude`, and host-specific root instruction files from
  the project layout. Do not add mirrors, generated copies, or symlinks for
  agent-specific skill directories.
- Keep the root `AGENTS.md` as the universal instruction entry point. Agents
  that do not discover `.agents/skills` natively must follow its explicit
  reading rules.
- Keep hard repository requirements in `AGENTS.md` and
  `Docs/Governance/AGENTS.md`; use skills for detailed, task-specific guidance.
- Preserve the Matt Pocock collection at release `1.1.0`, pinned to commit
  `2ab958093e83e0ec752e6c1c5932da465bf23e0c`, with its MIT license.

## Consequences

**Positive:**

- Every project skill has one canonical, version-controlled location.
- No host-specific skill copy can drift from another copy.
- The repository layout follows the open Agent Skills convention used by
  Codex and other compatible hosts.
- Future agent products can adopt the same directory without a migration.

**Negative:**

- Agent hosts that do not scan `.agents/skills` cannot expose these skills
  through their native skill selectors today.
- Those hosts depend on `AGENTS.md` and manual file reading until they support
  the canonical directory.
- Removing the host-specific root file may require users of that host to
  confirm that it respects `AGENTS.md`.

## Alternatives considered

1. **Keep `.cursor/skills` for C++ rules.** Rejected because it leaves two
   project-skill sources and makes the rules appear Cursor-specific.
2. **Duplicate skills under every host directory.** Rejected because copies
   drift and increase maintenance and review noise.
3. **Use symlinks into `.agents/skills`.** Rejected because the project supports
   Windows, where repository symlink behavior and permissions vary.
4. **Use only `AGENTS.md`, with no skills.** Rejected because loading every
   detailed workflow on every task wastes context and removes explicit skill
   invocation.

## References

- [Project skill catalog](../../.agents/skills/README.md)
- [Agent Skills specification](https://agentskills.io/specification)
