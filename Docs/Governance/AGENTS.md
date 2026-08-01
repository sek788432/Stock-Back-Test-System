# AGENTS.md — Playbook for AI Coding Assistants

This file is read by AI agents (Cursor, Codex, Claude Code, GitHub Copilot Workspace, etc.) at the start of any task. **Read it before doing anything in this repo.**

The repository root has a thin [`AGENTS.md`](../../AGENTS.md) pointer for tools that auto-discover; this file is the canonical playbook.

Humans contributing to this repo: see [`CONTRIBUTING.md`](CONTRIBUTING.md) and [`../ONBOARDING.md`](../ONBOARDING.md). Most of what's here applies to you too.

---

## 1. The reading order

Whenever you start a task here, read these in order. Don't skip — every section builds on the last.

1. **This file** (you're reading it).
2. **[`README.md`](../../README.md)** — what the project is.
3. **[`Docs/Specs/00_Overview.md`](../Specs/00_Overview.md)** — system architecture and end-to-end flow.
4. **The relevant `Docs/Specs/0X_*.md`** (numbers `01`–`11`) for the module you're touching — use **`11_Stock_Screener_KLine_Product.md`** when changing replay, authoring surfaces, or screener scope. Full index in [`Docs/Specs/README.md`](../Specs/README.md).
5. **[`.agents/skills/`](../../.agents/skills/)** — the repository's only project-skill directory, containing both repository-specific C++ rules and shared engineering and productivity workflows. Hosts that do not auto-discover this convention must still read a relevant `SKILL.md` when its description matches the task. Repository instructions take precedence over skill guidance.
6. **[`Docs/DEFINITION_OF_DONE.md`](../DEFINITION_OF_DONE.md)** — what "done" means in this repo. **You do not declare a task done until every box on this checklist is true.**
7. **[`Docs/Decisions/`](../Decisions/)** — Architecture Decision Records. Read the ADRs that touch your area before making design choices.

---

## 2. Hard rules (non-negotiable)

These are repo-wide invariants. Violating any of them is a defect.

| #   | Rule                                                                                                                                                                                                                                                                                                          |
| --- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| H1  | Never commit secrets. `.env` is gitignored; the only acceptable secret file in the tree is `.env.example` with placeholder values.                                                                                                                                                                            |
| H2  | Never edit `StockData/MarketData.duckdb` from C++ code. The Python pipeline owns writes; the C++ app is read-only against it (`Docs/Specs/04`).                                                                                                                                                               |
| H3  | Never break determinism. Engine runs with the same inputs must produce byte-identical outputs (`Docs/Specs/07` §8). If your change intentionally alters semantics, refresh the determinism fixture in the same PR and explain why.                                                                            |
| H4  | Never add `new` / `delete` / `malloc` / `free` to C++. Use RAII (`unique_ptr`, `shared_ptr`, `jthread`, `scoped_lock`). See skill `cpp-thread-safety`.                                                                                                                                                        |
| H5  | Never throw exceptions across module boundaries. Return `bte::core::Result<T, Error>` (`Docs/Specs/03` §6).                                                                                                                                                                                                   |
| H6  | Never use `using namespace std;` anywhere. Never use C-style casts in C++ (`(int)x`). See skill `cpp-modern-style`.                                                                                                                                                                                           |
| H7  | Never write a test that passes trivially. The CI's anti-cheat audit (`Docs/Specs/10` §5) will reject `EXPECT_TRUE(true)`, `EXPECT_EQ(x, x)`, empty test bodies, tautologies, mocking the unit under test, and silent skips. Don't try to satisfy the gate; satisfy the underlying intent (test the behavior). |
| H8  | Never claim a task is done until the Definition of Done passes ([`../DEFINITION_OF_DONE.md`](../DEFINITION_OF_DONE.md)). "I think it works" is not done.                                                                                                                                                      |
| H9  | Never add a dependency without justifying it in the PR description, naming the package and version, and confirming its license is compatible (see §6 below).                                                                                                                                                  |
| H10 | Never disable a CI gate to land your change. Use the documented exemption mechanism (`Docs/Specs/10` §10) which requires a CODEOWNER review.                                                                                                                                                                  |
| H11 | Never invent file paths, class names, or library APIs. If you're unsure something exists, search the repo or read the docs. Hallucinated symbols are caught by `cpp-modern-style` + `cpp-static-analysis` but waste reviewer time.                                                                            |
| H12 | Never silently change indentation, line endings, or formatting outside your diff. Run `clang-format` / `ruff format` only on touched files.                                                                                                                                                                   |

---

## 3. The work loop

Every task — feature, bug fix, refactor, docs change — follows this loop:

```
1. Read context           (this file + relevant Specs + relevant Skills)
2. Plan                   (state your plan back; for non-trivial work, propose an ADR)
3. Make the change        (small, focused commits; one concern per PR)
4. Add / update tests     (every public symbol; non-cheating; see `Docs/Specs/10` §5)
5. Run local gates        (format, tidy, ctest, sanitizers — `Docs/Specs/10` §3)
6. Verify Definition of Done passes
7. Open PR with the template filled out completely
8. Address review         (push fixes; do not force-push the PR branch unless asked)
9. Auto-merge once green and approved
```

**Don't skip step 1.** If your context window is tight, prefer re-reading the targeted spec sections over guessing.

**Don't skip step 6.** If you can't tick every DoD box, the task isn't done — narrow scope or ask for help.

---

## 4. PR conventions

Branch names: `feature/<short-name>`, `fix/<short-name>`, `docs/<short-name>`, `refactor/<short-name>`, `chore/<short-name>`.

Commit messages: **Conventional Commits**.

```
feat(engine): add nextBarOpen fill model
fix(data): handle multi-schema symbols in CSV adapter
docs(specs): clarify Bar.isValid invariants
test(indicators): cover RSI Wilder smoothing edge cases
refactor(strategy): extract Evaluator from RuleStrategy
chore(ci): bump clang-tidy to 18
perf(engine): avoid std::function in per-bar callback
```

Squash-merge on landing; the squashed commit message is the conventional message.

PR title = top commit's conventional message. PR body = [`.github/PULL_REQUEST_TEMPLATE.md`](../../.github/PULL_REQUEST_TEMPLATE.md), **fully filled out**, including the Definition of Done section (no leaving boxes blank — explain N/A explicitly).

For AI-authored PRs (Codex, Claude Code, Copilot Workspace, or similar), the PR
body must follow the same template standard as human-authored PRs:

- keep every template heading unless it is explicitly marked as deletable;
- write `N/A — <reason>` instead of leaving placeholders blank;
- for docs-only PRs, mark `docs — documentation only` and state `N/A — docs-only`
  for build, test, lint, performance, and code-specific DoD sections;
- include concrete reviewer focus points when the PR is a planning/spec PR;
- do not open a PR with the raw template comments still acting as the only
  content for a section.

One concern per PR. If you found unrelated bugs while working, file issues; don't smuggle them in.

---

## 5. When to write an ADR

Open an Architecture Decision Record ([`../Decisions/`](../Decisions/)) **before** writing code if your change:

- introduces a new third-party dependency,
- changes a public API that crosses a module boundary,
- changes the public plugin or Lua API (`Docs/Specs/08`),
- changes the data layer's contract with the Python pipeline (`Docs/Specs/04`),
- changes any CI gate (`Docs/Specs/10`),
- adds a new module to the dependency graph (`Docs/Specs/01` §1),
- has more than one reasonable answer.

The ADR template lives at [`../Decisions/0001-record-architecture-decisions.md`](../Decisions/0001-record-architecture-decisions.md). Number it sequentially. Reference it from your PR.

For small mechanical changes (typos, version bumps with no API change, refactors that preserve behavior), an ADR is not needed.

---

## 6. Adding dependencies

Default answer: **don't add one**. The C++20 standard library is large; the existing vcpkg manifest is curated.

If you must:

1. Add an ADR (see §5).
2. Verify the license is compatible:
   - **Allowed**: MIT, BSD (2/3-clause), Apache-2.0, MPL-2.0, ISC, Boost, zlib, LGPL (dynamically linked only).
   - **Forbidden without an explicit team decision**: GPL-2.0, GPL-3.0, AGPL, SSPL, custom "non-commercial" licenses.
3. Add to `vcpkg.json` (C++) or `requirements.txt` (Python), pinning to an exact version.
4. Add a one-line entry in [`../Decisions/dependencies.md`](../Decisions/dependencies.md) (`name | version | license | reason`).

---

## 7. Testing rules (the anti-cheat policy in plain language)

[`../Specs/10_CI_Dev_Flow.md`](../Specs/10_CI_Dev_Flow.md) §5 has the formal definitions. The short version: **a test must be able to fail when the code is wrong**. Mechanical ways to violate that, all rejected:

```cpp
EXPECT_TRUE(true);                          // (a) trivial
EXPECT_EQ(getX(), getX());                  // (b) tautology
TEST(Foo, doesNothing) {}                   // (c) empty
TEST(Foo, ignoresSelf) { int x = 1+1; ... } // (d) doesn't reference unit under test
EXPECT_CALL(mockSma, value()).WillOnce(...) // (e) mocking the unit under test
TEST(Foo, DISABLED_real)                    // (f) silent disable, no ISSUE-### justification
```

CI's mutation testing (`Docs/Specs/10` §6) catches the subtle version: a test that runs but doesn't actually check the right thing. If the mutation kill rate on your diff is < 70%, the PR fails — add tests until it clears.

When you write a test, **also write down what mutation it would catch**. If you can't think of one, the test is probably weak. (Examples: "this catches an off-by-one in the warm-up window count", "this catches flipping `<=` to `<` in the cross detector".)

---

## 8. When you don't know

Default to the safer choice:

- Don't know which header to put a public type in? → public `Include/Bte/<Module>/`. It's easier to demote later than promote.
- Don't know whether to use `unique_ptr` or `shared_ptr`? → `unique_ptr`. Refactor only when shared ownership is required.
- Don't know if a function should be in `Core` or `Data`? → put it where its dependencies live (`Docs/Specs/01` §1 graph).
- Don't know whether to write a test? → write one. The bar is "every public symbol has a test" (`Docs/Specs/10` §7).
- Don't know if a change needs an ADR? → write a short one. ADRs are cheap.
- Don't know what "done" looks like? → re-read [`../DEFINITION_OF_DONE.md`](../DEFINITION_OF_DONE.md).

If, after reading the relevant docs, you still don't know — **ask** in the PR description or as a draft PR. Don't guess and ship.

---

## 9. What you must NOT do

In addition to the hard rules in §2:

- Do **not** add hidden behavior to make tests pass (e.g. special-casing test inputs in production code).
- Do **not** rewrite git history of a PR branch after a reviewer has looked at it. Push fix commits; squash happens at merge.
- Do **not** edit files outside the scope of the task to "improve them" without asking. Stay in your lane.
- Do **not** pull the latest version of a dependency "to keep things current". Pinned versions are pinned for reasons.
- Do **not** commit binary artifacts (compiled binaries, generated CSVs, screenshots > 1 MB). Use git-lfs or external storage if needed.
- Do **not** disable, weaken, or work around CI gates. The gate exists so we don't ship broken software.
- Do **not** comment-out failing tests "to fix later". Either fix the test, fix the code, or open an issue and use the documented `DISABLED_` + `ISSUE-NNN` annotation.
- Do **not** declare a task done without the Definition of Done.

---

## 10. Communication norms (hybrid team)

This is a small (4–8 people) hybrid team. Defaults:

| Topic                               | Channel                                                                                                 |
| ----------------------------------- | ------------------------------------------------------------------------------------------------------- |
| Bug report                          | GitHub Issue (template)                                                                                 |
| Feature request                     | GitHub Issue (template)                                                                                 |
| Design proposal                     | ADR PR in [`../Decisions/`](../Decisions/)                                                              |
| Code change                         | PR with template filled                                                                                 |
| Question about a spec / ADR         | GitHub Discussion **or** comment on the spec/ADR file                                                   |
| Quick clarification                 | Sync chat (whatever the team uses) — but if it shaped a decision, write it down in an ADR or PR comment |
| Outage / something broken on `main` | Sync chat first, then issue with `priority:high`                                                        |

The weekly sync is for ambiguous questions and roadmap. Anything decided there must be backfilled into an ADR or PR before EOD that day. **Verbal decisions don't exist.**

---

## 11. Self-check before opening a PR

Quick mental pass. If you can answer "yes" to all, you're ready:

- [ ] I read the relevant `Docs/Specs/0X_*.md` for the area I changed.
- [ ] My change respects the hard rules in §2.
- [ ] Every public symbol I added or changed has a unit test that names it.
- [ ] My tests would actually fail if the production code were wrong (mutation-aware).
- [ ] I ran format + tidy + ctest locally.
- [ ] My commit messages are Conventional Commits.
- [ ] I filled out the PR template completely (no blank fields).
- [ ] I worked through the Definition of Done.
- [ ] I added or updated an ADR if the change qualifies (§5).
- [ ] I followed the relevant project skills in [`../../.agents/skills/`](../../.agents/skills/) and did not introduce any banned patterns from the repository-specific C++ skills there.

If yes, ship it.
