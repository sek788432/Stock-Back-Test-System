# 0004 — Anti-cheat CI gate with mutation testing

- **Status**: Accepted
- **Date**: 2026-05-06
- **Deciders**: founding team
- **Supersedes**: —
- **Superseded by**: —

## Context

A "tests passed" green check is necessary but not sufficient evidence that a change is correct. Common failure modes:

- Tests written that pass trivially (`EXPECT_TRUE(true)`).
- Tests that mock the unit under test and verify only the mock.
- Tests that run real code but don't actually assert against the right thing.
- Public symbols added with no test at all (gap caught later, in production).

In an AI-heavy authoring team, these failure modes are *more* common, not less, because AI agents optimize for the visible signal ("CI green") faster than humans do.

The CI pipeline needs gates that make the green check actually mean what it suggests.

## Decision

Adopt a multi-layer test-quality gate enforced in CI on every PR:

1. **Symbol-coverage parity (G4)**: every public C++ symbol declared in `Src/**/Include/**/*.h` and every public Python name in `DataFetcher/` must appear referenced from at least one test source. Tooling: `Tools/BteSymbolAudit.py` (libclang-based). Exemptions live in `Tools/SymbolAuditExemptions.yaml` with mandatory `until:` dates.
2. **Diff coverage (G5)**: ≥ 90% line coverage and ≥ 80% branch coverage on lines this PR adds or modifies. Computed by `diff-cover` over LCOV reports unified across LLVM, OpenCppCoverage, and `coverage.py`.
3. **Anti-cheat audit (G3)**: a libclang-based linter (`Tools/BteTestAudit.py`) walks test source ASTs and rejects seven concrete cheating patterns: trivial assertions, tautologies, empty test bodies, tests that don't reference their unit under test, mocking the unit under test, silent skips without `ISSUE-NNN` justification, and dead loops.
4. **Mutation testing (G6)**: `mull` (C++) and `mutmut` (Python) on changed files only, with a kill-rate threshold of ≥ 70%. Catches the *semantic* cheats that static rules can't: tests that run but don't actually verify anything.

Full description in [`../Specs/10CiDevFlow.md`](../Specs/10CiDevFlow.md) §3–§7.

## Consequences

**Positive:**

- "CI green" implies "tests would catch a real regression", not just "no test crashed".
- New public symbols can't slip through without coverage.
- Test debt becomes visible (exemption file, weekly report).
- AI-generated tests get audited at the same bar as human-written tests.

**Negative:**

- Higher CI time per PR (mutation testing is the expensive job).
- Authors occasionally hit false-positive rejections (rare, escape hatches exist).
- 70% mutation kill rate is a guess; will need calibration over the first month.
- Three custom-built tools (`BteTestAudit`, `BteSymbolAudit`, the mull driver) become a maintenance surface.

**Mitigations:**

- Mutation runs only on changed files, capped at 200 mutants per PR. Sampling beyond that.
- Escape hatches are explicit (`// BTE-AUDIT: smoke`, `// BTE-AUDIT: skip-justified ISSUE-NNN`, exemption files with `until:` dates and CODEOWNER review) so legitimate cases land without long arguments.
- Tools live in `Tools/` with their own tests; treated as first-class production code.
- Re-evaluate the 70% threshold after one month of live PRs. Document the calibration in a follow-up ADR.

## Alternatives considered

1. **Whole-project coverage threshold** (e.g. "must stay above 80%"). Rejected: punishes existing technical debt, doesn't speak to *this PR's* quality, and authors game it by adding cheap tests on already-tested code.
2. **No mutation testing**. Rejected: static rules catch only the obvious cheats. Without mutation, an AI agent (or hurried human) can pass G3 with a test that doesn't actually verify the behavior.
3. **Manual review as the only test-quality check**. Rejected: doesn't scale even at 4–8 people, and human reviewers miss subtle patterns the audit catches reliably.
4. **Stryker / PIT-style mutation runners**. Considered: mature in JS/Java worlds. We picked `mull` for C++ because LLVM-IR-level mutation integrates with our compile-commands.json toolchain; `mutmut` for Python because it's simple and fast.
5. **Property-based testing (rapidcheck) instead of mutation**. Considered: complementary, not a replacement. Property tests cover invariants well; mutation testing catches all weak tests, not just invariant-shaped ones. We may add property tests later.
6. **AI-as-reviewer scoring tests**. Considered: useful as advice, not as a gate. AI judgment is too unstable to use as a merge requirement.

## References

- [`../Specs/10CiDevFlow.md`](../Specs/10CiDevFlow.md)
- `Tools/BteTestAudit.py` (anti-cheat audit)
- `Tools/BteSymbolAudit.py` (symbol parity)
- [mull project](https://github.com/mull-project/mull)
- [mutmut](https://github.com/boxed/mutmut)
