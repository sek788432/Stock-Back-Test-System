# 0020 — Expand C++ skill standards enforcement

- **Status**: Accepted
- **Date**: 2026-08-22
- **Deciders**: repository maintainer
- **Supersedes**: —
- **Superseded by**: —

## Context

The five repository C++ skills describe modern style, OOP design, performance,
static analysis, and thread safety. The required full-tree project-standards
gate directly enforced only a subset of their unambiguous language rules. A
green result could therefore miss mechanically recognizable uses of unscoped
enums, C arrays and casts, legacy threading primitives, or missing
`#pragma once`.

The skills also contain contextual guidance that a line-oriented checker cannot
prove. Examples include whether an abstraction has earned its interface,
whether a copy occurs in a measured hot path, whether a lock covers I/O, and
whether profiling evidence supports an optimization. Treating those judgments
as regular-expression rules would produce false confidence and false positives.

Because merge-blocking rules must not depend on skill activation alone, promote
the directly enforced subset into the canonical governance hard rules as part
of the same change.

## Decision

Expand the merge-blocking C++ quality stack in layers. The full-tree
`Tools/CheckProjectStandards.py` gate directly rejects the following
project-owned C++ constructs:

- unscoped enums, recognized built-in C-style casts, C arrays, `goto`, and
  function-like macros;
- `std::auto_ptr`, `std::thread`, `pthread_create`, detached threads, manual
  mutex locking, `volatile`, and `std::vector<bool>`;
- the existing raw allocation, C string/output, `NULL`, `typedef`, and
  `using namespace std` violations; and
- project-owned source headers without `#pragma once`.

The checker also:

- validates direct `Bte/<Module>/...` includes against the implemented module
  dependency direction;
- rejects broad, unnamed, or unreasoned `NOLINT` directives; and
- invokes pinned clang-format 18 in dry-run/error mode for every project C++
  source blob from the submitted revision.

Strengthen the compiler warning baseline and add the selected
`misc-const-correctness`, `misc-definitions-in-headers`, and
`misc-header-include-cycle` clang-tidy checks. Add separate merge-blocking
ASan/UBSan/LSan and TSan CTest jobs; the runtimes remain separate because ASan
and TSan cannot be combined.

Record these direct bans in `Docs/Governance/AGENTS.md`; the skills continue to
provide their task-specific rationale and preferred replacements.

Mask C++ comments and string/character literals before applying token rules so
policy explanations and diagnostics do not become findings. Keep focused
positive, negative, and boundary tests for every new rule family.

Do not claim that the line-oriented checker proves every contextual statement
in the skills. The required full-project clang-tidy, cppcheck, IWYU, and
scan-build job remains the mechanical authority for AST and data-flow findings.
Runtime behavior covered only by sanitizers is owned by the sanitizer matrix,
and context-sensitive OOP and performance decisions remain review
requirements. Clazy, CodeQL, and a custom Clang AST policy plugin remain
Planned / not merge-blocking because they add distinct dependency, baseline,
and policy-design concerns that require focused changes.

## Consequences

**Positive:**

- CI directly blocks a substantially broader common subset of the five C++
  skill rule sets across the complete committed tree.
- Formatting drift, suppression hygiene, reverse module includes, and runtime
  sanitizer findings become explicit merge failures.
- Literal masking makes the expanded token checks safer than raw substring
  matching.
- The CI specification states exactly which skill concerns are direct,
  analyzer-backed, local-only, or review-only.

**Negative:**

- The standards checker still does not parse C++, so custom-type C-style casts
  and semantic ownership mistakes depend on the analyzer job or review.
- Tightening a direct full-tree rule requires cleaning any existing baseline in
  the same change.
- The two sanitizer configurations add build/test time to every workflow run.
- Skill guidance and merge gates remain separate policy surfaces that reviewers
  must keep aligned.

## Alternatives considered

1. **Claim that regular expressions enforce every skill statement.** Rejected
   because design quality, hot-path relevance, ownership, and profiling evidence
   are semantic and contextual.
2. **Run the compiler analyzers inside `CheckProjectStandards.py`.** Rejected
   because the existing required static-analysis job owns the configured Qt
   build, pinned tools, compilation database, and report artifacts.
3. **Leave all additional checks to clang-tidy.** Rejected because direct rules
   produce faster, clearer diagnostics and cover headers or spellings that do
   not need compilation context.
4. **Add Clazy, CodeQL, and a custom AST plugin in the same gate change.**
   Rejected because Qt-specific analysis, hosted security analysis, and a
   maintained compiler plugin each need independent dependency/license review,
   baseline tests, and ownership decisions.

## References

- [ADR 0006](0006-full-tree-project-standards-gate.md)
- [ADR 0019](0019-run-full-static-analysis-for-every-gate.md)
- [`../Specs/10CiDevFlow.md`](../Specs/10CiDevFlow.md)
- [`../../Tools/CheckProjectStandards.py`](../../Tools/CheckProjectStandards.py)
- [Project C++ skills](../../.agents/skills/README.md)
