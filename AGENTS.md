# AGENTS — pointer

The canonical playbook for AI coding agents in this repository lives at:

> **[`Docs/Governance/AGENTS.md`](Docs/Governance/AGENTS.md)**

This file exists at the repository root so AI tools that auto-discover
`AGENTS.md` immediately pick up the pointer and follow it. Host adapters such as
[`CLAUDE.md`](CLAUDE.md) import the same canonical playbook for tools that use a
different instruction entry point.

## Mandatory before any change in this repo

1. Open and **fully read** [`Docs/Governance/AGENTS.md`](Docs/Governance/AGENTS.md). It is non-optional.
2. Read the relevant module spec under [`Docs/Specs/`](Docs/Specs/README.md).
3. Read the relevant project [`.agents/skills/*/SKILL.md`](.agents/skills/) rules.
4. Confirm the work passes [`Docs/DefinitionOfDone.md`](Docs/DefinitionOfDone.md) before declaring a task complete.

If you are an AI agent reading this and you have not yet opened `Docs/Governance/AGENTS.md`, **stop and read it now**. The full rules, hard prohibitions, work loop, and PR conventions are there. This pointer is not a substitute.
