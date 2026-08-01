# Claude Code project instructions

@Docs/Governance/AGENTS.md

The imported playbook above is the canonical repository instruction source and
is mandatory for every task.

- Read the relevant module spec under `Docs/Specs/` before making a change.
- Inspect `.agents/skills/` and fully read every `SKILL.md` whose description
  matches the task. This is the repository's only project-skill directory.
- Treat the hard rules in `Docs/Governance/AGENTS.md` as non-negotiable. Skills
  provide detailed workflows and may not weaken or override those rules.
- Confirm `Docs/DEFINITION_OF_DONE.md` before declaring work complete.

Do not copy project rules into this file. Keep it as a Claude Code adapter so
all agents continue to use the same canonical instructions.
