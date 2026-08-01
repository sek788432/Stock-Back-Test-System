# Contributing

Welcome. This is a private team repository (4–8 contributors). Contributions come from invited collaborators and AI agents (Cursor, Codex, Claude Code, etc.) supervised by humans.

If you're an **AI agent**, your primary playbook is [`AGENTS.md`](AGENTS.md). Everything below applies to you too, but `AGENTS.md` is more specific about how you should behave.

If you're a **human contributor**, read this file and [`../Onboarding.md`](../Onboarding.md) first. The full design lives in [`../Specs/`](../Specs/README.md).

---

## TL;DR

1. Pick or open an issue.
2. Branch from `main`: `feature/<short-name>`, `fix/<short-name>`, etc.
3. Make small, focused commits with [Conventional Commits](https://www.conventionalcommits.org/) messages.
4. For non-trivial changes: open an [ADR](../Decisions/) first.
5. Write or update unit tests for every public symbol you touch (`Docs/Specs/10`).
6. Run local gates: `clang-format`, `clang-tidy`, `ctest --preset dev`.
7. Open a PR using the template; fill every section.
8. Confirm [`../DefinitionOfDone.md`](../DefinitionOfDone.md) passes.
9. Wait for CI green + 1 review approval. Auto-merge takes it from there.

---

## Where things live

| You want to... | Read |
|---|---|
| Set up your dev environment | [`../Onboarding.md`](../Onboarding.md) |
| Understand the architecture | [`../Specs/00Overview.md`](../Specs/00Overview.md) |
| Find the spec for a module | [`../Specs/README.md`](../Specs/README.md) |
| Know what coding rules apply | [`../../.agents/skills/README.md`](../../.agents/skills/README.md) |
| Know what "done" looks like | [`../DefinitionOfDone.md`](../DefinitionOfDone.md) |
| Review someone's PR | [`../ReviewPlaybook.md`](../ReviewPlaybook.md) |
| Cut a release | [`../ReleaseProcess.md`](../ReleaseProcess.md) |
| Record a design decision | [`../Decisions/`](../Decisions/) |
| Configure AI assistants | [`AGENTS.md`](AGENTS.md) |

---

## Branching and commits

- `main` is always green and shippable. Direct pushes are blocked.
- Feature branches are short-lived (target: < 5 days).
- Rebase your branch on `main` before requesting review if it lags by more than a few days.
- We squash-merge. Your PR title becomes the squash commit message.

Conventional Commit types we use:

| Type | Use for |
|---|---|
| `feat` | new user-visible feature |
| `fix` | bug fix |
| `docs` | documentation only |
| `test` | tests only, no production change |
| `refactor` | non-behavioral code restructure |
| `perf` | performance improvement (must include benchmark numbers in body) |
| `chore` | tooling, deps, CI, build |

Example body for a `perf` change:

```
perf(indicators): avoid heap alloc in RSI update path

Before: 124 ns/op (nanobench, M1, release)
After:   38 ns/op
Verified determinism fixture unchanged.
```

---

## Code style

The two binding sources are:

1. The five repository-specific C++ skills in [`../../.agents/skills/`](../../.agents/skills/README.md): modern C++, thread safety, performance, OOP/design, static analysis.
2. The repo's `.clang-format` and `.clang-tidy` (authoritative for any conflict).

Naming (recap from `cpp-modern-style` and [`../Specs/03BackendCore.md`](../Specs/03BackendCore.md) §1):
- Variables / methods / namespaces: `lowerCamelCase`.
- Types: `UpperCamelCase`.
- Private members: trailing underscore.
- Project-owned file stems: **PascalCase** (e.g. `Bar.h`, `ReplayPlan.md`).
- C++ unit test files: **`UnitTest_<Thing>.cpp`** (e.g. `UnitTest_Bar.cpp`).
- Project-owned directories: **PascalCase** for repo layout and code roots (e.g. `Src/`, `Docs/`, `Docs/Governance/`, `Tests/`; CMake binary dir `Output/` per `CMakePresets.json`).
- Exact external-tool conventions, numbered ADR slugs, entrypoints, unit-test names, and domain-data identifiers are the documented exceptions (ADR 0010).
- Unit-test suites live under `Tests/Unit/<Module>/`; shared fixtures remain under `Tests/Fixtures/`.

---

## Testing rules (recap from `Docs/Specs/10`)

A PR must:

- Add a test for every new public symbol.
- Achieve ≥ 90% diff line coverage and ≥ 80% diff branch coverage.
- Pass the anti-cheat audit (no trivial / tautological / empty / disabled-without-issue tests).
- Achieve ≥ 70% mutation kill-rate on changed files.

If a test is genuinely a smoke test, mark it: `// BTE-AUDIT: smoke`. If a test must be skipped, mark it: `// BTE-AUDIT: skip-justified ISSUE-NNN`.

---

## Reviewing

We expect a **24-hour first response** on any PR during business days, even if it's just "I'll get to it tomorrow". Hybrid team — async first, but don't leave a PR hanging.

Reviewer checklist is in [`../ReviewPlaybook.md`](../ReviewPlaybook.md). Authors should self-review using the same checklist before requesting review.

---

## Communication

| What | Where |
|---|---|
| Bug | GitHub Issue (`bug` template) |
| Feature idea | GitHub Issue (`feature` template) |
| Design discussion | ADR PR in [`../Decisions/`](../Decisions/) |
| Code change | Pull Request |
| Question about a spec | Comment on the spec file in a PR or issue |
| Quick chat | Team sync chat — but **if it shaped a decision, write it down in an ADR or PR comment.** Verbal decisions don't exist. |
| Outage on `main` | Sync chat first, issue with `priority:high` immediately after |

The weekly sync is for roadmap and ambiguous questions. Decisions made there are backfilled into ADRs or PR comments same day.

---

## Adding a dependency

See [`AGENTS.md` §6](AGENTS.md). Default answer is no. If yes, ADR + license check + version pin + entry in [`../Decisions/Dependencies.md`](../Decisions/Dependencies.md).

---

## Security / secrets

- Never commit secrets. `.env` is gitignored; `.env.example` is the canonical placeholder.
- Databento API keys: each contributor uses their own key in their local `.env`.
- Code-signing keys, GPG keys, GitHub Actions secrets: managed by the repo lead. Don't reference them from code paths a fork could hit.

If you accidentally commit a secret: rotate it immediately, then `git filter-repo` (or contact the lead) to scrub history. Tell the team.

---

## License

See [`LICENSE`](LICENSE). This repo is private; the team picks the license collectively. Unless and until that decision is made, treat all code as "all rights reserved" within the team.

---

## Questions?

If something here is unclear, that's a doc bug. Open a PR fixing it.
