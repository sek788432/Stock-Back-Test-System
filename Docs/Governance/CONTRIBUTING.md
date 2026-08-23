# Contributing

Welcome. This is a private team repository (4–8 contributors). Contributions come from invited collaborators and AI agents (Cursor, Codex, Claude Code, etc.) supervised by humans.

If you're an **AI agent**, your primary playbook is [`AGENTS.md`](AGENTS.md). Everything below applies to you too, but `AGENTS.md` is more specific about how you should behave.

If you're a **human contributor**, read this file and [`../Onboarding.md`](../Onboarding.md) first. The full design lives in [`../Specs/`](../Specs/README.md).

---

## TL;DR

1. Pick or open an issue.
2. Branch from `main`: `feature/<short-name>`, `fix/<short-name>`, etc.
3. Make small, focused commits with [Conventional Commits](https://www.conventionalcommits.org/) messages.
4. Resolve every ambiguity with the maintainer. Update the [living
   important-decisions document](../Decisions/ImportantDecisions.md) first when
   the choice is hard to reverse, surprising without context, and a real
   trade-off; implemented merge-gate changes always require an entry.
5. Add positive, negative, and boundary unit tests for every affected public
   behavior; bug fixes and behavior changes also need regression tests
   ([`../Specs/10CiDevFlow.md`](../Specs/10CiDevFlow.md)).
6. Run the applicable verified local commands from
   [`../DefinitionOfDone.md`](../DefinitionOfDone.md).
7. Open a PR using the template; fill every section.
8. Confirm [`../DefinitionOfDone.md`](../DefinitionOfDone.md) passes.
9. Wait for the required CI and reviews configured in GitHub. Repository files
   do not prove the current branch-protection or auto-merge settings (`Specs/10`).

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
| Record an important design decision | [`../Decisions/ImportantDecisions.md`](../Decisions/ImportantDecisions.md) |
| Configure AI assistants | [`AGENTS.md`](AGENTS.md) |

---

## Branching and commits

- Keep `main` green. Public release remains blocked by the explicit data and
  packaging prerequisites in the specs; branch-protection and direct-push
  settings are external and must be checked in GitHub.
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

The binding sources are:

1. The repository hard rules in [`AGENTS.md`](AGENTS.md).
2. The repository-specific C++ skills in [`../../.agents/skills/`](../../.agents/skills/README.md).

The checked-in `.clang-tidy` configuration and full-tree clang-format check are
enforced by merge-blocking CI jobs (`Specs/10`).

Naming (recap from `cpp-modern-style` and [`../Specs/03BackendCore.md`](../Specs/03BackendCore.md) §1):
- Variables / methods / namespaces: `lowerCamelCase`.
- Types: `UpperCamelCase`.
- Private members: trailing underscore.
- New C++ file stems: **UpperCamelCase** (e.g. `Bar.h`, `Bar.cpp`).
- C++ unit test files: **`UnitTest_<Thing>.cpp`** (e.g. `UnitTest_Bar.cpp`).
- **Directories (new top-level or module folders):** **UpperCamelCase** for repo layout and code roots (e.g. `Src/`, `Docs/`, `Docs/Governance/`, `Tests/`; CMake binary dir `Output/` per `CMakePresets.json`).
- **`*.md`:** no enforced filename pattern.

---

## Testing rules

A PR must:

- Add positive, negative, and meaningful boundary unit tests for every affected
  public behavior.
- Add a regression test for every bug fix and intentional public-behavior
  change.
- Exercise real production behavior with meaningful assertions; no trivial,
  tautological, empty, silently disabled, or mock-only substitute tests.
- Add contract/integration coverage for IPC, snapshot generation, and
  `.bteresult` changes.

Changed-line and changed-branch coverage are merge gates. Semantic anti-cheat,
public-behavior parity, and mutation enforcement remain planned. See
[`../Specs/10CiDevFlow.md`](../Specs/10CiDevFlow.md) for exact status.

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
| Design discussion | Issue or PR discussion; update [`../Decisions/ImportantDecisions.md`](../Decisions/ImportantDecisions.md) when the three-part §5 threshold applies |
| Code change | Pull Request |
| Question about a spec | Comment on the spec file in a PR or issue |
| Quick chat | Team sync chat — but **if it shaped an important decision, update the living document through a reviewed change.** Verbal decisions don't exist. |
| Outage on `main` | Sync chat first, issue with `priority:high` immediately after |

The weekly sync is for roadmap and ambiguous questions. Important decisions
made there are backfilled into the living document through a reviewed change
the same day.

---

## Adding a dependency

See [`AGENTS.md` §6](AGENTS.md). Default answer is no. If yes: maintainer
approval, an important-decision update when the choice meets §5, a license
check, an exact version pin, and an entry in
[`../Decisions/Dependencies.md`](../Decisions/Dependencies.md).

---

## Security / secrets

- Never commit secrets. `.env` is gitignored. If a tracked placeholder template
  is added, it must be `.env.example` and contain no real credentials.
- Databento API keys: each contributor uses their own key in their local `.env`.
- Code-signing keys, GPG keys, GitHub Actions secrets: managed by the repo lead. Don't reference them from code paths a fork could hit.

If you accidentally commit a secret: rotate it immediately, then `git filter-repo` (or contact the lead) to scrub history. Tell the team.

---

## License

Project source is licensed under Apache-2.0. See the canonical
[`../../LICENSE`](../../LICENSE). Third-party dependencies and market data keep
their own terms; the project source license does not grant redistribution rights
for Databento-derived data.

---

## Questions?

If something here is unclear, that's a doc bug. Open a PR fixing it.
