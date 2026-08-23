# Copilot pull-request review instructions

Before reviewing, read and follow `Docs/Governance/AGENTS.md`, the relevant
`Docs/Specs/` documents, `Docs/Decisions/ImportantDecisions.md`,
`Docs/DefinitionOfDone.md`, and any matching `.agents/skills/*/SKILL.md`. Until
issue #53 phase 3 completes, also use `Docs/Decisions/README.md` to read every
still-active portion of accepted numbered decisions relevant to the touched
area, whether or not an owning spec links it. Those files are authoritative;
this host adapter does not redefine their rules.

Report only actionable correctness, safety, standards, and regression findings.
Give each finding a severity, explain the failure scenario, and point to the
smallest relevant file and line range. Do not treat a green CI result as proof
that the implementation is correct.
