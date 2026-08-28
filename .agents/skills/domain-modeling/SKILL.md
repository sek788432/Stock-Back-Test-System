---
name: domain-modeling
description: Build and sharpen a project's domain model. Use when the user wants to pin down domain terminology or a ubiquitous language, record an architectural decision, or when another skill needs to maintain the domain model.
---

# Domain Modeling

Actively build and sharpen the project's domain model as you design. This is the *active* discipline — challenging terms, inventing edge-case scenarios, and writing the glossary and decisions down the moment they crystallise. (Merely *reading* `CONTEXT.md` for vocabulary is not this skill — that's a one-line habit any skill can do. This skill is for when you're changing the model, not just consuming it.)

## File structure

This repository has one shared context:

```
/
├── CONTEXT.md
├── Docs/
│   └── Decisions/
│       └── ImportantDecisions.md
└── Src/
```

Create context files lazily — only when you have something to write. If no
`CONTEXT.md` exists, create one when the first term is resolved. Important
cross-cutting decisions belong in the repository's single living decisions
document rather than context-local or numbered archives.

## During the session

### Challenge against the glossary

When the user uses a term that conflicts with the existing language in `CONTEXT.md`, call it out immediately. "Your glossary defines 'cancellation' as X, but you seem to mean Y — which is it?"

### Sharpen fuzzy language

When the user uses vague or overloaded terms, propose a precise canonical term. "You're saying 'account' — do you mean the Customer or the User? Those are different things."

### Discuss concrete scenarios

When domain relationships are being discussed, stress-test them with specific scenarios. Invent scenarios that probe edge cases and force the user to be precise about the boundaries between concepts.

### Cross-reference with code

When the user states how something works, check whether the code agrees. If you find a contradiction, surface it: "Your code cancels entire Orders, but you just said partial cancellation is possible — which is right?"

### Update CONTEXT.md inline

When a term is resolved, update `CONTEXT.md` right there. Don't batch these up — capture them as they happen. Use the format in [CONTEXT-FORMAT.md](./CONTEXT-FORMAT.md).

`CONTEXT.md` should be totally devoid of implementation details. Do not treat `CONTEXT.md` as a spec, a scratch pad, or a repository for implementation decisions. It is a glossary and nothing else.

### Offer important-decision entries sparingly

Only offer to add or update a living decision entry when all three are true:

1. **Hard to reverse** — the choice materially constrains future work and would
   have a meaningful cost to change.
2. **Surprising without context** — a future contributor could not reliably
   infer the rationale from the owning spec and implementation.
3. **Real trade-off** — more than one reasonable answer existed and the
   rejected alternatives remain important to understand.

If any of the three is missing, skip the entry. When all three apply, reach
shared understanding with the maintainer and update the existing living
document in place. Use the format in
[IMPORTANT-DECISION-FORMAT.md](./IMPORTANT-DECISION-FORMAT.md). Remove an entry
when it no longer constrains the project; Git history preserves chronology.
An implemented merge-gate change always requires an entry under repository
governance. For another listed category that does not pass all three tests,
explain why an entry is unnecessary in the PR.
