# 0007 — Vendor shared agent skills

- **Status**: Accepted
- **Date**: 2026-08-01
- **Deciders**: project maintainers
- **Supersedes**: —
- **Superseded by**: —

## Context

The repository already carries project-specific C++ rules under
`.cursor/skills`, but those rules are tied to one agent host. Contributors use
Codex, Cursor, Claude Code, and other AI tools, so user-profile installations
produce inconsistent workflows and are not available to every contributor.

Matt Pocock's skill collection provides reusable engineering and productivity
workflows under the MIT license. Adopting it is a third-party tooling decision,
so its exact source, scope, and update policy must be recorded.

## Decision

Vendor the 22 stable Engineering and Productivity skills from
`mattpocock/skills` release `1.1.0`, pinned to commit
`2ab958093e83e0ec752e6c1c5932da465bf23e0c`, under `.agents/skills`.

- Do not vendor upstream's deprecated, in-progress, personal, or miscellaneous
  groups.
- Keep the upstream MIT license beside the vendored skills.
- Require every AI agent to consult relevant shared skills through the
  canonical repository playbook, even if its host does not automatically
  discover `.agents/skills`.
- Keep `.cursor/skills` as the repository-specific source of C++ rules.
- Treat the vendored files as a pinned snapshot. Updates require review and a
  normal project commit; no command may update them automatically.
- Repository governance, specs, and Definition of Done take precedence when a
  vendored skill conflicts with local rules.

The skills are documentation and workflow assets only. No package is installed
into the application, build, test, or runtime dependency graph.

## Consequences

**Positive:**

- All contributors and AI agent hosts receive the same reviewable workflows.
- Skills remain available offline and do not depend on a personal home
  directory.
- Pinning makes changes reproducible and prevents silent upstream drift.

**Negative:**

- The repository owns periodic review and manual refreshes of the snapshot.
- Hosts without native `.agents/skills` discovery rely on the playbook's
  explicit fallback instruction.
- Vendored guidance can overlap local rules; precedence must remain explicit.

## Alternatives considered

1. **Install into each contributor's user profile.** Rejected because the
   project could not guarantee availability or a common version.
2. **Run `npx skills@latest` on demand.** Rejected because it is unpinned and
   introduces network-dependent, machine-specific state.
3. **Duplicate the collection under every host-specific directory.** Rejected
   because multiple copies would drift and triple review noise.
4. **Commit cross-platform symlinks for each host.** Rejected because symlink
   behavior and permissions are inconsistent across the supported operating
   systems.

## References

- [Matt Pocock skills repository](https://github.com/mattpocock/skills)
- [Vendored provenance and license](../../.agents/skills/README.md)
