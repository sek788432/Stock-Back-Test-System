# 0008 — Standardize project agent skills

- **Status**: Accepted
- **Date**: 2026-08-01
- **Deciders**: project maintainers
- **Supersedes**: 0007
- **Superseded by**: —

## Context

The repository stored its five C++ workflow skills under `.cursor/skills` and
the vendored Matt Pocock collection under `.agents/skills`. The two skill
locations duplicated concepts, made the source of truth unclear, and coupled
project guidance to a particular agent product. Some agent products also use
their own root instruction entry point, so a small compatibility adapter is
still useful as long as it does not become another source of project rules.

The open Agent Skills format makes each skill portable, but agent products do
not all discover the same filesystem locations. The project prefers one clean,
host-neutral source over native discovery in every current tool.

## Decision

Use `.agents/skills` as the repository's only project-skill directory.

- Move the five repository-specific C++ skills into `.agents/skills` beside
  the vendored engineering and productivity skills.
- Put all future skills, including `stockbt-actions`, under `.agents/skills`.
- Remove `.cursor`, `.claude`, and every mirrored or generated copy of project
  skills. Do not add symlinks for agent-specific skill directories.
- Keep a thin root `CLAUDE.md` adapter because Claude Code natively loads that
  file. It imports the canonical playbook and points Claude to `.agents/skills`;
  it must not duplicate hard rules or skill contents. Equivalent thin adapters
  may be added for other hosts only when needed for reliable discovery.
- Keep the root `AGENTS.md` as the universal instruction entry point. Agents
  that do not discover `.agents/skills` natively must follow its explicit
  reading rules.
- Keep hard repository requirements in `Docs/Governance/AGENTS.md`; use root
  instruction files only to load that playbook and use skills for detailed,
  task-specific guidance.
- Preserve the Matt Pocock collection at release `1.1.0`, pinned to commit
  `2ab958093e83e0ec752e6c1c5932da465bf23e0c`, with its MIT license.

## Consequences

**Positive:**

- Every project skill has one canonical, version-controlled location.
- No host-specific skill copy can drift from another copy.
- The repository layout follows the open Agent Skills convention used by
  Codex and other compatible hosts.
- Claude Code reliably loads the same canonical playbook through its native
  `CLAUDE.md` project-memory mechanism.
- Future agent products can adopt the same directory without a migration.

**Negative:**

- Agent hosts that do not scan `.agents/skills` cannot expose these skills
  through their native skill selectors today.
- Those hosts depend on `AGENTS.md` and manual file reading until they support
  the canonical directory.
- Each retained host adapter adds a small maintenance obligation and must be
  kept free of duplicated policy.

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
5. **Remove every host-specific root adapter.** Rejected because host support
   for `AGENTS.md` and `.agents/skills` is not universal. A thin import adapter
   improves discovery without creating a second policy source.

## References

- [Project skill catalog](../../.agents/skills/README.md)
- [Agent Skills specification](https://agentskills.io/specification)
- [Claude Code project memory and imports](https://docs.anthropic.com/en/docs/claude-code/memory)
