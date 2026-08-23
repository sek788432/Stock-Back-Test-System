# AGENTS.md — Playbook for AI Coding Assistants

This file is read by AI agents (Cursor, Codex, Claude Code, GitHub Copilot Workspace, etc.) at the start of any task. **Read it before doing anything in this repo.**

The repository root has a thin [`AGENTS.md`](../../AGENTS.md) pointer for tools
that auto-discover it. Host-specific adapters such as
[`CLAUDE.md`](../../CLAUDE.md) import this file when a tool uses a different
instruction entry point. Those adapters must remain thin and must not duplicate
project rules. This file is the canonical playbook.

Humans contributing to this repo: see [`CONTRIBUTING.md`](CONTRIBUTING.md) and [`../Onboarding.md`](../Onboarding.md). Most of what's here applies to you too.

---

## 1. The reading order

Whenever you start a task here, read these in order. Don't skip — every section builds on the last.

1. **This file** (you're reading it).
2. **[`README.md`](../../README.md)** — what the project is.
3. **[`Docs/Specs/Overview.md`](../Specs/Overview.md)** — system architecture and end-to-end flow.
4. **The relevant descriptive specification** for the module you are touching.
   Use the exact current filenames in
   [`Docs/Specs/README.md`](../Specs/README.md), including
   **`BacktestReplayProduct.md`** for strategy, Backtest, Result, or Replay UI.
5. **[`.agents/skills/`](../../.agents/skills/)** — the repository's only project-skill directory, containing both repository-specific C++ rules and shared engineering and productivity workflows. Hosts that do not auto-discover this convention must still read a relevant `SKILL.md` when its description matches the task. Repository instructions take precedence over skill guidance.
6. **[`Docs/DefinitionOfDone.md`](../DefinitionOfDone.md)** — what "done" means in this repo. **You do not declare a task done until every box on this checklist is true.**
7. **[`Docs/Decisions/ImportantDecisions.md`](../Decisions/ImportantDecisions.md)** — the single living record of important decisions that still constrain the project.

---

## 2. Hard rules (non-negotiable)

These are repo-wide invariants. Violating any of them is a defect.

This section is the authoritative source for hard requirements. Skills under
`.agents/skills/` explain how to apply these requirements in specific tasks,
but a skill may not weaken or override them. When guidance should become a new
hard rule, promote it into this section and add mechanical CI enforcement where
practical; do not rely on skill activation alone.

| #   | Rule                                                                                                                                                                                                                                                                                                          |
| --- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| H1  | Never commit secrets. `.env` is gitignored; the only acceptable secret file in the tree is `.env.example` with placeholder values.                                                                                                                                                                            |
| H2  | Never mutate project-managed market data from the application. V1 runtime reads immutable, release-built snapshots derived from `StockData/Extracted`; snapshot creation is a separate release process (`Docs/Specs/DataLayer.md`). |
| H3  | Never break functional determinism. Identical immutable inputs must produce identical canonical functional records and the same `canonicalResultHash` (`Docs/Specs/EngineReplayPnL.md` §9). SQLite page layout, local paths, wall-clock metadata, and other non-functional container bytes are excluded. Any intentional semantic change requires a regression test, an explained canonical-fixture update once that gate exists, and reviewer approval. |
| H4  | Never add `new` / `delete` / `malloc` / `free` to C++. Use RAII (`unique_ptr`, `shared_ptr`, `jthread`, `scoped_lock`). See skill `cpp-thread-safety`.                                                                                                                                                        |
| H5  | Never throw exceptions across module boundaries. Return the repository's `bte::core::Result<T>` error contract (`Docs/Specs/BackendCore.md` §6). |
| H6  | Never use `using namespace std;` anywhere. Never use C-style casts in C++ (`(int)x`). See skill `cpp-modern-style`.                                                                                                                                                                                           |
| H7  | Never write a test that passes trivially. Tests must exercise real production behavior and contain meaningful assertions. The implemented standards checker rejects only the limited patterns documented in `Docs/Specs/CiDevFlow.md`; semantic anti-cheat and mutation checks are planned, not current merge gates. |
| H8  | Never claim a task is done until the Definition of Done passes ([`../DefinitionOfDone.md`](../DefinitionOfDone.md)). "I think it works" is not done.                                                                                                                                                      |
| H9  | Never add a dependency without justifying it in the PR description, naming the package and version, and confirming its license is compatible (see §6 below).                                                                                                                                                  |
| H10 | Never disable, bypass, or weaken an implemented merge gate to land a change. A change to a merge gate requires an approved entry in the living important-decisions document, a focused tooling change, and review; there is no undocumented override. |
| H11 | Never invent file paths, class names, library APIs, commands, tools, or CI checks. Search the checked-out repository and distinguish implemented behavior from planned design. |
| H12 | Never silently change indentation, line endings, or formatting outside your diff. Run `clang-format` / `ruff format` only on touched files.                                                                                                                                                                   |
| H13 | Project-owned C++ headers use `#pragma once` and repository naming/layout. Do not use C arrays, `NULL`, `typedef`, unscoped enums, C stdio/string APIs, C-style casts, `goto`, function-like macros, `std::auto_ptr`, `std::vector<bool>`, or output parameters in project-owned interfaces. Isolate and document narrow exceptions required by `main`, an external C ABI, or a framework boundary. |
| H14 | Use `std::chrono` for time and `std::filesystem::path` for paths in project-owned C++ APIs. Mark fallible `Result`-returning APIs `[[nodiscard]]`; never use `errno` as a module error contract.                                                                                                                     |
| H15 | Never use manual mutex `lock()`/`unlock()`, `std::thread`, `pthread_create`, detached threads, or `volatile` for synchronization. Never call `QWidget` methods from a worker thread. Use scoped locks, `std::jthread` with cancellation, immutable/value snapshots, and queued Qt delivery.                                                        |
| H16 | Static-analysis and sanitizer suppressions must be narrow, name the exact check, include a reason, and have maintainer approval. Never blanket-disable a check or suppress a sanitizer finding merely to make a gate pass.                                                                                       |
| H17 | Project-owned directory names and file stems use PascalCase. Exact external-tool conventions, `UnitTest_<Thing>` files, entrypoints, and domain-data identifiers are the documented exceptions; the rationale is recorded in the [living decision](../Decisions/ImportantDecisions.md#agent-authority-and-repository-layout). Every unit-test suite lives under `Tests/Unit/<Module>/`. |
| H18 | Before pushing or otherwise requesting CI for a change that touches C++ code, build wiring, C++ tests, or C++ quality tooling, run `./RunTest.sh` and then `./RunQuality.sh --base <base-revision> --head HEAD`. Both must pass on the exact committed `HEAD` being submitted. Do not use a CI run as the first quality check. |

---

## 3. The scope-sensitive work loop

Use the smallest loop that matches the requested authority. Do not turn an
inspection request into a code change or a documentation request into an
unrequested implementation.

1. **Read context.** Read this file, the relevant specs and active important decisions, the matching
   skills, and the Definition of Done.
2. **Classify the task.** Decide whether it is inspection/advice, documentation,
   code behavior, bug fix, refactor, CI/tooling, or release work.
3. **Inspect before asserting.** Search the current tree, including relevant
   uncommitted files. Do not infer that a documented tool or feature exists.
4. **Plan proportionately.** State a short plan for non-trivial work. Obtain the
   maintainer decision and update the living important-decisions document first
   when §5 requires it.
5. **Change only the authorized scope.** Preserve unrelated and user-owned
   changes.
6. **Test according to impact.**
   - Inspection/advice: no mutation; report evidence.
   - Docs-only: validate links, examples, status labels, and contradictions.
   - New or changed public behavior: add positive, negative, and boundary unit
     tests for every affected public behavior.
   - Bug fix or intentional behavior change: add a regression test that fails
     under the previous behavior.
   - IPC, snapshot generation, persistence formats, and other cross-module
     contracts: add contract or integration tests in addition to unit tests.
7. **Run applicable verified checks.** Use only commands that exist in the
   checked-out tree. For applicable C++ changes, run `./RunTest.sh` and the
   complete `./RunQuality.sh --base <base-revision> --head HEAD` workflow before
   pushing or requesting CI. `Docs/Specs/CiDevFlow.md` separates current merge gates from
   planned checks.
8. **Verify the applicable Definition of Done.** Mark non-applicable items with
   a reason; never claim an unrun or nonexistent check passed.
9. **Hand off clearly.** Summarize changed files, verification performed, and
   any planned or blocked work that remains.

If an applicable requirement cannot be satisfied, narrow the work, record the
gap accurately, or ask for help. Do not silently downgrade the requirement.

---

## 4. PR conventions

Branch names: `feature/<short-name>`, `fix/<short-name>`, `docs/<short-name>`, `refactor/<short-name>`, `chore/<short-name>`.

Commit messages: **Conventional Commits**.

```
feat(engine): add nextBarOpen fill model
fix(data): handle multi-schema symbols in CSV adapter
docs(specs): clarify Bar.isValid invariants
test(indicators): cover RSI Wilder smoothing edge cases
refactor(strategy): extract Evaluator from SelectableConditions
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

## 5. When to record an important decision

The project keeps one living record at
[`../Decisions/ImportantDecisions.md`](../Decisions/ImportantDecisions.md).
Use the `grilling` workflow to resolve every ambiguous product, compatibility,
retention, or specification choice with the maintainer before implementation.

Add or update an important-decision entry only when all three tests pass:

1. **Hard to reverse:** the choice materially constrains future work and would
   have a meaningful cost to change.
2. **Surprising without context:** a future contributor could not reliably
   infer the rationale from the owning spec and implementation.
3. **Real trade-off:** more than one reasonable answer existed and the rejected
   alternatives remain important to understand.

Apply the tests especially when a change:

- introduces a new third-party dependency,
- changes a public API that crosses a module boundary,
- changes the public Python strategy API (`Docs/Specs/StrategyAuthoring.md`),
- changes the immutable market-snapshot contract (`Docs/Specs/DataLayer.md`),
- changes any CI gate (`Docs/Specs/CiDevFlow.md`),
- adds a new module to the dependency graph (`Docs/Specs/Architecture.md` §2),
- has more than one reasonable answer.

An implemented merge-gate change always requires an entry under H10. For any
other listed category that does not pass all three tests, explain why an entry
is unnecessary in the PR.

Each entry contains only the current decision, why it was chosen, important
rejected alternatives and why, consequences, and the owning specification.
Update an entry in place when the current decision changes. Remove it when it
no longer constrains the project; Git history preserves chronology. Do not add
numbered, append-only, superseded, or deprecated decision files.

For small mechanical changes (typos, version bumps with no API change,
behavior-preserving refactors), an important-decision entry is not needed.

---

## 6. Adding dependencies

Default answer: **don't add one**. The C++20 standard library is large. This
repository currently pins C++ dependencies through checked-in CMake/
`FetchContent` configuration and Python dependencies through workflow-specific
requirements files; no `vcpkg.json` exists.

If you must:

1. Add or update an important-decision entry when the dependency choice meets
   the threshold in §5.
2. Verify the license is compatible:
   - **Allowed**: MIT, BSD (2/3-clause), Apache-2.0, MPL-2.0, ISC, Boost, zlib, LGPL (dynamically linked only).
   - **Forbidden without an explicit team decision**: GPL-2.0, GPL-3.0, AGPL, SSPL, custom "non-commercial" licenses.
3. Update the actual owning dependency file discovered in the checkout, pinning
   an immutable CMake source revision or exact Python version and regenerating
   any checked-in hash lock through its documented workflow.
4. Add a one-line entry in [`../Decisions/Dependencies.md`](../Decisions/Dependencies.md) (`name | version | license | reason`).

---

## 7. Testing rules

[`../Specs/CiDevFlow.md`](../Specs/CiDevFlow.md) defines the formal
contract and its current enforcement status. Every affected public behavior
requires all three categories below:

- **Positive:** valid input produces the specified result.
- **Negative:** invalid input or a rejected operation produces the specified
  error and no forbidden side effect.
- **Boundary:** values at and immediately around meaningful limits behave as
  specified. If a behavior truly has no meaningful boundary, state why in the
  test or PR evidence.

Every bug fix and every intentional change to public behavior also requires a
focused regression test that fails under the old implementation. One test may
cover more than one category when its assertions clearly prove each case.

A test must be able to fail when production behavior is wrong. Prohibited
patterns include:

```cpp
EXPECT_TRUE(true);                          // (a) trivial
EXPECT_EQ(getX(), getX());                  // (b) tautology
TEST(Foo, doesNothing) {}                   // (c) empty
TEST(Foo, ignoresSelf) { int x = 1+1; ... } // (d) doesn't reference unit under test
EXPECT_CALL(mockSma, value()).WillOnce(...) // (e) mocking the unit under test
TEST(Foo, DISABLED_real)                    // (f) silent disable, no ISSUE-### justification
```

The current line-oriented standards checker catches only a subset of these
patterns. Changed-line and changed-branch coverage are mechanically enforced;
semantic anti-cheat, public-behavior parity, and mutation testing remain
target-state checks. Review tests for intent instead of treating green
mechanical checks as proof.

---

## 8. When you don't know

Default to the safer choice:

- Don't know which header to put a public type in? → the owning module's
  `Src/<Module>/Include/Bte/<Module>/` tree. Search the checked-out module layout
  before naming a new path.
- Don't know whether to use `unique_ptr` or `shared_ptr`? → `unique_ptr`. Refactor only when shared ownership is required.
- Don't know if a function should be in `Core` or `Data`? → put it where its dependencies live (`Docs/Specs/Architecture.md` §2 graph).
- Don't know whether to write a test? → write one. The bar is positive,
  negative, and boundary unit coverage for every affected public behavior
  (`Docs/Specs/CiDevFlow.md`).
- Don't know if a choice belongs in the living important-decisions document? →
  stop and ask the maintainer; do not create a historical record by default.
- Don't know what "done" looks like? → re-read [`../DefinitionOfDone.md`](../DefinitionOfDone.md).

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
| Design proposal                     | Issue or PR discussion, then an approved update to [`../Decisions/ImportantDecisions.md`](../Decisions/ImportantDecisions.md) when §5 applies |
| Code change                         | PR with template filled                                                                                 |
| Question about a spec / decision    | GitHub Discussion **or** comment on the owning spec or important-decisions entry                         |
| Quick clarification                 | Sync chat (whatever the team uses) — but if it shaped an important decision, update the living document or record it in the PR pending approval |
| Outage / something broken on `main` | Sync chat first, then issue with `priority:high`                                                        |

The weekly sync is for ambiguous questions and roadmap. Any important decision
made there must be added to the living document through a reviewed change
before EOD that day. **Verbal decisions don't exist.**

---

## 11. Self-check before opening a PR

Quick mental pass. If you can answer "yes" to all, you're ready:

- [ ] I read the relevant descriptive spec using its exact filename from
  [`../Specs/README.md`](../Specs/README.md).
- [ ] My change respects the hard rules in §2.
- [ ] Every affected public behavior has positive, negative, and boundary unit tests.
- [ ] Every bug fix or intentional public-behavior change has a regression test.
- [ ] My tests would actually fail if the production code were wrong (mutation-aware).
- [ ] I ran every applicable command that exists in this checkout and recorded any planned check that is not yet available.
- [ ] Before requesting CI for applicable C++ work, `./RunTest.sh` and the complete `./RunQuality.sh --base <base-revision> --head HEAD` passed on the submitted commit.
- [ ] My commit messages are Conventional Commits.
- [ ] I filled out the PR template completely (no blank fields).
- [ ] I worked through the Definition of Done.
- [ ] I added or updated the living important-decisions document if the change qualifies (§5).
- [ ] I followed the relevant project skills in [`../../.agents/skills/`](../../.agents/skills/) and did not introduce any banned patterns from the repository-specific C++ skills there.

If yes, ship it.
