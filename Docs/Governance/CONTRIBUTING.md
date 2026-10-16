# Contributing

Welcome. Contributions may come from human collaborators or AI agents working
under human direction; both follow the same repository contracts and review
requirements.

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
   ([`../Specs/CiDevFlow.md`](../Specs/CiDevFlow.md)).
6. Run the applicable verified local commands from
   [`../DefinitionOfDone.md`](../DefinitionOfDone.md).
7. Open a PR using the template; fill every section.
8. Confirm [`../DefinitionOfDone.md`](../DefinitionOfDone.md) passes.
9. Wait for the required CI and reviews configured in GitHub. Repository files
   do not prove the current branch-protection or auto-merge settings (`Specs/CiDevFlow.md`).

---

## Where things live

| You want to... | Read |
|---|---|
| Set up your dev environment | [`../Onboarding.md`](../Onboarding.md) |
| Understand the architecture | [`../Specs/Overview.md`](../Specs/Overview.md) |
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

Example body for a `perf` change (use the actual checked-in benchmark or
documented measurement; the numbers and tool label below are illustrative):

```
perf(indicators): avoid heap alloc in RSI update path

Before: 124 ns/op (documented benchmark, Apple Silicon, release preset)
After:   38 ns/op
Verified determinism fixture unchanged.
```

---

## Code style

The binding sources are:

1. The repository hard rules in [`AGENTS.md`](AGENTS.md).
2. The repository-specific C++ skills in [`../../.agents/skills/`](../../.agents/skills/README.md).

The checked-in `.clang-tidy` configuration and full-tree clang-format check are
enforced by merge-blocking CI jobs (`Specs/CiDevFlow.md`).

Naming (recap from `cpp-modern-style` and [`../Specs/BackendCore.md`](../Specs/BackendCore.md) §2):
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
[`../Specs/CiDevFlow.md`](../Specs/CiDevFlow.md) for exact status.

---

## Reviewing

Repository files do not promise a response-time SLA. Keep review requests and
responses in the PR, with the complete template and required checks visible.

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
| Quick clarification | Issue or PR discussion on the affected work |
| Something broken on `main` | Bug issue with reproduction evidence and impact |

Important decisions are not complete until the owning spec and living decision
record, when required, are updated through review.

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
- Databento API keys: each contributor exports their own key only in the local
  shell that runs the ingestion command.
- Code-signing keys and GitHub Actions secrets belong in repository/CI secret
  storage. Do not expose them to fork-controlled code paths.

If you accidentally commit a secret, rotate it immediately and notify the
repository maintainer through a private security channel. History rewriting is
a coordinated incident response, not an ordinary contributor command.

---

## License

Project source is licensed under Apache-2.0. See the canonical
[`../../LICENSE`](../../LICENSE). Third-party dependencies and market data keep
their own terms; the project source license does not grant redistribution rights
for Databento-derived data.

---

## Questions?

If something here is unclear, that's a doc bug. Open a PR fixing it.
