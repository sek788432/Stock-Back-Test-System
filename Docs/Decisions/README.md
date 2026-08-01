# Architecture Decision Records (ADRs)

ADRs are short, dated, immutable records of a design decision and its trade-offs. They explain *why* the codebase looks the way it does, so that future contributors (humans and AI) don't re-litigate decided questions or unknowingly undo them.

This is the team's source of truth for design rationale. **Verbal decisions don't exist** — if it shaped the code, it's an ADR (or a comment on one).

---

## When to write an ADR

Open an ADR PR **before** the implementation if your change:

- introduces a third-party dependency,
- changes a public API across module boundaries,
- changes the plugin or Lua API ([`../Specs/08`](../Specs/08_Plugin_System.md)),
- changes the data layer's contract with the Python pipeline ([`../Specs/04`](../Specs/04_Data_Layer.md)),
- changes any CI gate ([`../Specs/10`](../Specs/10_CI_Dev_Flow.md)),
- adds a module to the dependency graph ([`../Specs/01`](../Specs/01_Architecture.md) §1),
- has more than one reasonable answer.

Skip the ADR for typos, version bumps that don't change APIs, mechanical refactors that preserve behavior, and one-off fixes.

If you're not sure, write one. ADRs are cheap (often < 80 lines) and pay back many times over in the next person's onboarding.

---

## How to write one

1. Copy `0001-record-architecture-decisions.md` as a starting template.
2. Number it `NNNN-` with the next free integer (zero-padded to 4 digits).
3. Use a short, hyphen-separated title.
4. Fill out **Context**, **Decision**, **Consequences**, and **Alternatives considered**.
5. Open as a PR. Reviewers focus on rationale, not prose.

---

## ADR statuses

- **Proposed** — under review.
- **Accepted** — merged. Becomes the project's position.
- **Superseded by NNNN** — replaced by a later ADR. **Don't edit the old ADR's Decision**; supersession is itself a record.
- **Deprecated** — no longer applicable, but no new decision yet.

ADRs are append-only. To change a decision, write a new ADR that supersedes it. Update the old ADR's status line, but **do not rewrite its Decision** — history matters.

---

## Index

| # | Title | Status |
|---|---|---|
| 0001 | [Record architecture decisions](0001-record-architecture-decisions.md) | Accepted |
| 0002 | [C++ and Qt as the desktop stack](0002-cpp-and-qt-as-the-desktop-stack.md) | Accepted |
| 0003 | [Hybrid rule + Lua strategy authoring](0003-hybrid-rule-and-lua-strategy-authoring.md) | Accepted |
| 0004 | [Anti-cheat CI gate with mutation testing](0004-anti-cheat-ci-gate-with-mutation-testing.md) | Accepted |
| 0005 | [Build, distribution, and launcher deployment decisions](0005-build-distribution-launcher-decisions.md) | Accepted |
| 0006 | [Full-tree project standards gate](0006-full-tree-project-standards-gate.md) | Accepted |

When you add an ADR, add a row here. Keep numbering monotonic — gaps are confusing, never reuse a number.

---

## Dependencies log

Per [`../Governance/AGENTS.md`](../Governance/AGENTS.md) §6, every dependency we adopt has a one-liner here: [`dependencies.md`](dependencies.md). It's not a full ADR (the ADR is the upstream decision); it's the rolling summary so anyone can answer "what do we depend on and why" in 30 seconds.
