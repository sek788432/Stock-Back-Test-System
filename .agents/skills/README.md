# Shared Agent Skills

This directory contains project-scoped skills shared by every AI agent working
in this repository. Agent hosts that support the Agent Skills convention should
discover these folders automatically. Other hosts must consult the relevant
`SKILL.md` when its description matches the current task, as required by
[`Docs/Governance/AGENTS.md`](../../Docs/Governance/AGENTS.md).

## Vendored collection

- Source: [mattpocock/skills](https://github.com/mattpocock/skills)
- Upstream release: `1.1.0`
- Pinned commit: `2ab958093e83e0ec752e6c1c5932da465bf23e0c`
- License: MIT; see [`LICENSE.mattpocock-skills`](LICENSE.mattpocock-skills)
- Included: the 22 stable skills in upstream's Engineering and Productivity
  catalogs
- Excluded: deprecated, in-progress, personal, and miscellaneous skills

These files are a vendored snapshot. They do not update automatically. Review
upstream changes and commit a new pinned snapshot deliberately.

The `setup-matt-pocock-skills` skill is installed but has not been run. Invoke
it explicitly if the repository should adopt the optional issue-tracker,
triage-label, and domain-document configuration used by that collection.

Repository instructions always take precedence over vendored skill guidance.
