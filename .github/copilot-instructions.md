# Copilot pull-request review instructions

Before reviewing, read and follow `Docs/Governance/AGENTS.md`, the relevant
`Docs/Specs/` documents and accepted ADRs, `Docs/DEFINITION_OF_DONE.md`, and any
matching `.agents/skills/*/SKILL.md`. Those files are authoritative; this host
adapter does not redefine their rules.

Report only actionable correctness, safety, standards, and regression findings.
Give each finding a severity, explain the failure scenario, and point to the
smallest relevant file and line range. Do not treat a green CI result as proof
that the implementation is correct.
