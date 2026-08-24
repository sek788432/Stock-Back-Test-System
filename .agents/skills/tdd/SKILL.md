---
name: tdd
description: Test-driven development. Use when the user wants to build features or fix bugs test-first, mentions "red-green-refactor", or wants integration tests.
---

# Test-Driven Development

TDD is the red → green loop. This skill is the reference that makes that loop produce tests worth keeping: what a good test is, where tests go, the anti-patterns, and the rules of the loop. Every section applies on every cycle — consult them before and during the loop, not after.

Read the owning spec, public header, existing registered tests, and active
important decisions so test names and expectations use the project's actual
domain language.

## What a good test is

Tests verify behavior through public interfaces, not implementation details. Code can change entirely; tests shouldn't. A good test reads like a specification — "user can checkout with valid cart" tells you exactly what capability exists — and survives refactors because it doesn't care about internal structure.

See [tests.md](tests.md) for examples and [mocking.md](mocking.md) for mocking guidelines.

## Seams — where tests go

A **seam** is the boundary where you observe behavior without coupling the test
to incidental structure. Prefer the implemented public interface. A private
helper may be tested directly only when that is the clearest way to prove
public behavior and the test remains stable under behavior-preserving changes.

Identify the public seam from the owning spec and implemented header. Ask the
maintainer only when choosing a seam would create a new public contract or
resolve a material ambiguity; otherwise use the narrowest existing observable
boundary.

## Anti-patterns

- **Implementation-coupled** — mocks internal collaborators, tests private methods, or verifies through a side channel (querying the database instead of using the interface). The tell: the test breaks when you refactor but behavior hasn't changed.
- **Tautological** — the assertion recomputes the expected value the way the code
  does (`EXPECT_EQ(add(a, b), a + b)`), so it passes by construction and can
  never disagree with the code. Expected values must come from an independent
  literal, worked example, or specification.
- **Horizontal slicing** — writing all tests first, then all implementation. Bulk tests verify _imagined_ behavior: you test the _shape_ of things rather than user-facing behavior, the tests go insensitive to real changes, and you commit to test structure before understanding the implementation. Work in **vertical slices** instead — one test → one implementation → repeat, each test a **tracer bullet** that responds to what the last cycle taught you.

## Rules of the loop

- **Red before green.** Write the failing test first, then only enough code to pass it. Don't anticipate future tests or add speculative features.
- **One slice at a time.** One seam, one test, one minimal implementation per cycle.
- **Refactor only while green.** After the minimal implementation passes, remove
  duplication and improve names or seams without changing behavior, then rerun
  the focused test before starting the next red case. Review remains read-only.
