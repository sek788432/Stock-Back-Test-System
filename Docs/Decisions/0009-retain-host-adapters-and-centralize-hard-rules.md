# 0009 — Retain host adapters and centralize hard rules

- **Status**: Accepted
- **Date**: 2026-08-01
- **Deciders**: project maintainers
- **Supersedes**: 0008
- **Superseded by**: —

## Context

ADR 0008 correctly established `.agents/skills` as the single project-skill
directory, but removing every host-specific instruction entry point makes
discovery less reliable for agents that do not natively load `AGENTS.md` or
`.agents/skills`. Claude Code natively loads `CLAUDE.md` and supports importing
another Markdown file with `@path` syntax.

The repository also describes some C++ rules as non-negotiable only inside
task-triggered skills. Skill activation is useful for detailed workflows, but
it is not a reliable authority boundary for hard rules.

## Decision

- Keep `.agents/skills` as the repository's only project-skill directory. Do
  not create host-specific copies, mirrors, generated copies, or symlinks.
- Keep root `AGENTS.md` as the universal pointer to the canonical playbook in
  `Docs/Governance/AGENTS.md`.
- Retain a thin root `CLAUDE.md` adapter that imports the canonical playbook and
  points to `.agents/skills`. It must not copy hard rules or skill contents.
- Permit an equivalent host adapter only when needed for reliable discovery.
  An adapter may provide role-specific invocation instructions, but it may not
  redefine repository policy.
- Keep every non-negotiable rule in `Docs/Governance/AGENTS.md`. Skills contain
  task-specific explanations, examples, and procedures; they may not weaken or
  override governance.
- Mechanically enforce hard rules in CI where practical. Documentation must
  describe enforcement as partial until all listed rules have a corresponding
  gate.

## Consequences

**Positive:**

- Claude Code reliably receives the same playbook as agents that load
  `AGENTS.md`.
- Project skills still have one host-neutral, version-controlled source.
- Hard rules no longer depend solely on skill activation.
- Thin adapters avoid policy drift while preserving product compatibility.

**Negative:**

- Each host adapter adds a small maintenance surface.
- Some hard rules still require human review until mechanical enforcement is
  implemented.
- The governance playbook grows when a skill rule is promoted to an invariant.

## Alternatives considered

1. **Remove all host adapters.** Rejected because discovery support is not
   uniform across agent products.
2. **Duplicate every skill under each host directory.** Rejected because copies
   drift and increase review noise.
3. **Keep hard rules only in C++ skills.** Rejected because task-triggered
   loading can be missed.
4. **Put all detailed skill content in governance.** Rejected because it would
   load irrelevant workflows on every task and waste context.

## References

- [Canonical agent playbook](../Governance/AGENTS.md)
- [Project skill catalog](../../.agents/skills/README.md)
- [Agent Skills specification](https://agentskills.io/specification)
- [Claude Code project memory and imports](https://docs.anthropic.com/en/docs/claude-code/memory)
