<!--
Fill every section. If something is N/A, write "N/A — <reason>".
Reviewers will not start until the template is fully filled.

If you're an AI agent: this is mandatory, not optional. See Docs/Governance/AGENTS.md §4 and §11.
-->

## Summary

<!-- 1–3 sentences. What changes and why. -->

## Linked issue / ADR

- Closes #
- Related ADR:

<!-- If this is a non-trivial change with no ADR, explain why no ADR is needed
(see AGENTS.md §5 for when an ADR is required). -->

## Type of change

<!-- Tick all that apply. -->

- [ ] feat — new user-visible feature
- [ ] fix — bug fix
- [ ] docs — documentation only
- [ ] test — tests only, no production change
- [ ] refactor — non-behavioral code restructure
- [ ] perf — performance (benchmark numbers below)
- [ ] chore — tooling, deps, CI, build

## What changed

<!-- A short list of the meaningful changes; not a copy of the diff. -->

-
-

## How I verified it

<!-- Concrete steps you took locally. Not a placeholder. -->

- [ ] `cmake --build --preset dev` clean
- [ ] `ctest --preset dev` passes (sanitizers clean)
- [ ] `Tools/RunClangTidyDiff.sh` clean on touched files
- [ ] `Tools/RunClangFormat.sh --check` clean
- [ ] If concurrency-touching: `ctest --preset dev-tsan` passes
- [ ] If hot-path touching: nanobench numbers in section below

## Performance impact

<!-- Required for any change in a documented hot path (engine bar loop,
indicators, replay, data prefetch). Otherwise write "N/A". -->

```
Before:  ___ ns/op  (nanobench, machine, build flags)
After:   ___ ns/op
```

## Tests added / changed

<!-- For each new or changed public symbol, link the test that covers it.
If you skipped tests, say why and reference the DoD section that allows it. -->

- `bte::module::Type::method` → `Tests/.../TypeTest.cpp::testCase`
-

## Anti-cheat self-check (Docs/Specs/10 §5)

- [ ] Every new test would actually fail if the production code were wrong (mentally apply mutation)
- [ ] No `EXPECT_TRUE(true)`, no tautological assertions, no empty test bodies
- [ ] No mocking the unit under test
- [ ] No `DISABLED_*` / `GTEST_SKIP()` without `ISSUE-NNN` justification

## Skills compliance

<!-- The five repository-specific .agents/skills/cpp-* skills enforce these. Confirm you didn't violate them. -->

- [ ] `cpp-modern-style` — no banned C-style idioms; naming follows lowerCamelCase / UpperCamelCase
- [ ] `cpp-thread-safety` — RAII; no raw new/delete; cross-thread state is immutable or owned by one thread
- [ ] `cpp-performance` — no new allocations or virtual/std::function calls in hot paths
- [ ] `cpp-oop-design` — single responsibility; module dependencies respected; no premature abstractions
- [ ] `cpp-static-analysis` — zero new warnings on touched files

## Definition of Done

<!-- Copy the relevant sections from Docs/DefinitionOfDone.md and tick boxes.
See: https://github.com/your-org/Stock-Back-Test-System/blob/main/Docs/DefinitionOfDone.md
Do NOT delete the section — leave the headings even if items are N/A. -->

### Universal

- [ ] Single concern; one PR.
- [ ] Branch name matches `feature|fix|docs|refactor|chore|perf/<short-name>`.
- [ ] Commit messages are Conventional Commits.
- [ ] No secrets committed; no commented-out code; no TODO without `ISSUE-NNN`.
- [ ] Self-reviewed using `Docs/ReviewPlaybook.md`.

### Code (delete if PR is docs-only)

- [ ] Builds clean; sanitizers clean; lint clean; format clean.
- [ ] Determinism fixture unchanged (or refreshed and explained).

### Tests

- [ ] Every new public symbol has a unit test.
- [ ] Diff coverage targets met (≥ 90% line, ≥ 80% branch).
- [ ] Mutation kill rate target met (≥ 70%).

### Docs

- [ ] Cross-references updated in any spec/ADR I changed.

## Risk and rollback

<!-- What's the worst-case if this lands? How does someone undo it? -->

- Risk:
- Rollback:

## For the reviewer

<!-- Optional: anything you want the reviewer to focus on. -->

-
