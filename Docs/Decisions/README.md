# Project Decisions

This directory contains two maintained documents:

- [`ImportantDecisions.md`](ImportantDecisions.md) — the single living record
  of important decisions that still constrain the project;
- [`Dependencies.md`](Dependencies.md) — the dependency, version, license, and
  usage inventory.

Before implementation, resolve material ambiguity with the maintainer through
the repository `grilling` workflow. Add or update a living entry only when all
three threshold tests in `ImportantDecisions.md` pass; an implemented
merge-gate change always requires one. Update the entry and owning specification
in the same reviewed change.

Git history is the archaeology source. Do not add numbered, append-only,
superseded, deprecated, or historical decision documents.
