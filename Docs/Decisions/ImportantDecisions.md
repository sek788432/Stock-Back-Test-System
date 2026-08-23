# Important Decisions

This is the project's single living record of design decisions that still
constrain current or approved implementation-ready work. It explains why the
owning specifications and implementation have their present shape without
duplicating their detailed contracts.

Issue #53 phase 2 adopts this policy. Phase 3 will migrate concise active
rationale from the numbered ADR files and then remove that archive. Until each
decision is migrated atomically with its consumers, the still-active portions
of accepted numbered decisions remain authoritative. Use the temporary index
in [`README.md`](README.md) to read every relevant decision for the touched
area, whether or not an owning spec already links it. Do not add or amend a
numbered ADR; obsolete and superseded chronology is not a current contract.

## Entry contract

Each retained decision has exactly these parts:

- **Current decision** — the constraint that applies now.
- **Why** — the reason it was chosen.
- **Important rejected alternatives** — only alternatives whose trade-offs
  remain useful, with the reason each was rejected.
- **Consequences** — the costs, limits, and follow-on obligations.
- **Owning specification** — the canonical contract that defines detailed
  behavior and implementation status.

## When an entry is required

Use the repository `grilling` workflow to resolve every ambiguous product,
compatibility, retention, or specification choice with the maintainer before
implementation.

Add or update an entry only when all three tests pass:

1. **Hard to reverse:** the choice materially constrains future work and would
   have a meaningful cost to change.
2. **Surprising without context:** a future contributor could not reliably
   infer the rationale from the owning spec and implementation.
3. **Real trade-off:** more than one reasonable answer existed and the rejected
   alternatives remain important to understand.

Apply these tests especially when a choice:

- introduces a third-party dependency;
- changes a public API across module boundaries;
- changes the public plugin or Python strategy API;
- changes the immutable market-snapshot contract;
- changes an implemented CI gate;
- adds a module to the dependency graph; or
- has more than one reasonable answer.

An implemented merge-gate change always requires an entry. For any other
listed category that does not pass all three tests, explain why an entry is
unnecessary in the PR.

Small mechanical edits, behavior-preserving refactors, and implementation
details already determined by an owning specification do not need an entry.

## Maintenance rules

- Update the applicable entry in place when the current decision changes.
- Update the owning specification in the same reviewed change.
- Remove an entry when it no longer constrains the project.
- Keep only active rationale and important trade-offs; Git history is the
  archaeology source.
- Do not create append-only, numbered, superseded, or deprecated decision
  records.
- Keep dependency version and license facts in
  [`Dependencies.md`](Dependencies.md); add an entry here only when the
  dependency choice meets the decision threshold above.
