---
name: review
description: Review a branch's complete change set—committed branch changes plus staged, unstaged, and untracked work—and its related implementation, documentation, tests, and build wiring. Apply all repository C++ skills, verify implementation-to-documentation alignment, evaluate test intent and behavioral coverage, and run every applicable test and checked-in review command. Use before a commit, pull request, merge, or handoff, or whenever the user asks for a comprehensive code, documentation, test, coverage, branch, or uncommitted-change review.
---

# Comprehensive Branch Review

Perform an evidence-based, read-only review. Do not modify code, documentation,
tests, configuration, or git state unless the user separately asks for fixes.

## 1. Establish the review range

Pin one fixed point before reviewing:

1. Use the commit, tag, or branch supplied by the user.
2. Otherwise use the current branch's upstream when it resolves.
3. Otherwise prefer `origin/main`, then local `main`.
4. If none resolves, ask for a fixed point instead of guessing.

Resolve the fixed point and compute its merge-base with `HEAD`. Record the exact
commit IDs. Treat the review range as the union of:

- committed changes from the merge-base through `HEAD`;
- staged changes;
- unstaged changes;
- untracked, non-ignored files.

Capture these separately with `git status --short`, `git log`, `git diff`,
`git diff --cached`, and `git ls-files --others --exclude-standard`. Also inspect
the combined effective tracked diff from the merge-base to the working tree.
Do not omit a file because it appears in more than one layer.

Stop and report the problem if the fixed point does not resolve. An empty
committed diff does not end the review when uncommitted or untracked changes
exist.

## 2. Build the impact closure

Start with every changed path, then inspect related content needed to judge the
change correctly:

- declarations, definitions, public interfaces, implementations, and callers;
- module dependencies and CMake/build/test registration;
- tests, fixtures, mocks, persisted schemas, and generated artifacts;
- relevant specs, active important decisions, README files, examples, and
  user-facing documentation;
- concurrency, ownership, error, determinism, and performance contracts touched
  indirectly by the change.

Use `rg` and repository indexes to find these relationships. Review the full
changed files and the necessary related sections, not only isolated diff hunks.
Keep unrelated pre-existing code outside the verdict unless the change relies
on it or makes its defect newly reachable.

## 3. Load authoritative guidance

Read the following before forming findings:

1. `Docs/Governance/AGENTS.md` and `Docs/DefinitionOfDone.md`.
2. `README.md`, `Docs/Specs/00Overview.md`, and each module spec affected by the
   impact closure.
3. Applicable entries in `Docs/Decisions/ImportantDecisions.md` and the
   originating issue, PRD, or user-supplied acceptance criteria when available.
4. For any C++ in the range or impact closure, fully read and apply all five:
   - `.agents/skills/cpp-modern-style/SKILL.md`
   - `.agents/skills/cpp-oop-design/SKILL.md`
   - `.agents/skills/cpp-performance/SKILL.md`
   - `.agents/skills/cpp-static-analysis/SKILL.md`
   - `.agents/skills/cpp-thread-safety/SKILL.md`

Repository governance, current specs, active important decisions, and the
checked-out implementation override stale or conflicting skill examples.
Verify every path, preset, script, and claimed gate in the checkout before
using it. Never describe planned tooling as implemented or merge-blocking.

## 4. Run three independent review axes

Use three parallel sub-agents when available; otherwise perform the axes
sequentially. Give each reviewer the pinned merge-base, review inventory, raw
diffs, relevant files, and authoritative sources. Do not give one reviewer
another reviewer's conclusions.

### Code axis

Review all changed production code and its impact closure for:

- functional correctness, failure paths, compatibility, and regressions;
- spec and active-decision conformance, module dependency direction, and public
  contracts;
- every applicable rule from the five C++ skills, including modern C++20 style,
  RAII, ownership, interface depth, composition, thread safety, error handling,
  complexity, allocation, hot paths, and deterministic behavior;
- secrets, unsafe suppression, dead code, invented APIs, and build wiring;
- warning-prone or analyzer-prone constructs that checked-in tools may miss.

Apply performance and concurrency rules based on impact, not merely on filename.
Require measurements for performance claims and sanitizer/TSan evidence when the
checked-out repository provides applicable commands.

### Documentation axis

Review every changed document plus related authoritative documentation. Check:

- each behavioral statement against the actual implementation and tests;
- Implemented, Planned, Blocked, local-only, and merge-blocking labels against
  the checked-out tree;
- names, paths, commands, defaults, examples, links, anchors, and diagrams;
- consistency among README files, specs, active important decisions, public
  headers, UI text, and test expectations;
- whether implementation changes require documentation updates, and whether
  documentation changes promise implementation that does not exist.

Treat higher-authority governance, specs, and active important decisions as
controlling. Flag contradictions in either direction.

### Tests and verification axis

Map every affected public behavior and defect fix to tests. For each behavior,
evaluate:

- positive, negative, and meaningful boundary cases;
- the specified error and absence of forbidden side effects;
- focused regression coverage for bug fixes and intentional behavior changes;
- contract/integration coverage for cross-module, IPC, snapshot, and persistence
  changes;
- test intent: real production behavior, meaningful assertions, deterministic
  setup, and the ability to fail when production behavior is wrong;
- registration in the build/test runner and consistency with documented intent.

Use a behavior-to-test matrix. Treat semantic coverage as a review obligation.
Run a coverage tool or quote a percentage only if a checked-in, documented
coverage mechanism exists in this checkout; otherwise report mechanical coverage
as unavailable and do not invent a threshold.

## 5. Run every applicable verified check

Discover commands from the checked-out repository and `Docs/Specs/10CiDevFlow.md`.
For the current repository, the baseline full registered-test workflow is:

```bash
cmake --preset qt-dev -DBTE_BUILD_TESTS=ON -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
cmake --build --preset qt-dev --parallel
ctest --preset qt-dev --no-tests=error
```

For applicable C++ changes, also run the checked-in sanitizer preset:

```bash
cmake --preset dev-sanitize
cmake --build --preset dev-sanitize --parallel
ctest --preset dev-sanitize --no-tests=error
```

List registered tests before execution when useful, and ensure `ctest` runs the
complete preset rather than a filtered subset. Run additional checked-in test
suites only after verifying their commands and registration. Do not assume a
DataFetcher, coverage, mutation, TSan, clang-tidy, cppcheck, scan-build, IWYU,
or format command exists merely because a document or another skill mentions it.

For committed revisions, run the standards checker with the pinned commits when
the command is supported:

```bash
python3 Tools/CheckProjectStandards.py --full-tree --base <merge-base> --head HEAD
```

Also audit added lines in tracked staged and unstaged content when the checkout's
checker supports working-tree mode:

```bash
python3 Tools/CheckProjectStandards.py --base <merge-base>
```

The full-tree command does not inspect uncommitted files, and the working-tree
diff command does not inspect untracked files. Review all remaining content
manually and with any verified non-mutating checks. In review mode, never run a
formatter or analyzer in fix mode.

Record each command, exit status, and concise result. Distinguish:

- **Passed** — executed completely with exit status zero;
- **Failed** — executed and returned a failure;
- **Blocked** — applicable but could not run, with the exact reason;
- **Not available** — no verified implementation exists in this checkout;
- **Not applicable** — explain why.

Do not claim that all tests pass if any applicable suite failed, was filtered,
or did not run. Continue useful independent review work after a failure when safe.

## 6. Validate and aggregate findings

Reproduce or corroborate every potential defect against the effective working
tree. Remove duplicates across axes. Keep findings that are specific, actionable,
and caused or exposed by the review range.

Order findings by severity:

- **P0** — catastrophic or security-critical; must stop shipment.
- **P1** — incorrect behavior, data loss, race, contract break, or required test
  failure; must fix before merge.
- **P2** — material maintainability, documentation, test-coverage, or performance
  gap; should fix before merge.
- **P3** — minor but concrete improvement.

For each finding provide the axis, file and tight line range, evidence, impact,
violated contract or rule, and a concise remediation. Do not inflate preferences
into defects.

## 7. Report the verdict

Lead with findings. If there are none, state that explicitly. Then provide:

1. **Range reviewed** — fixed point, merge-base, `HEAD`, working-tree layers,
   and impacted areas.
2. **Findings** — ordered P0 through P3 with clickable file references.
3. **Behavior-to-test coverage** — positive, negative, boundary, regression,
   and contract/integration status per affected behavior.
4. **Documentation alignment** — implementation-to-doc and doc-to-implementation
   result.
5. **Verification matrix** — exact commands and Passed/Failed/Blocked/Not
   available/Not applicable status.
6. **Definition of Done gaps** — every unsatisfied applicable item.
7. **Verdict** — `Pass`, `Needs changes`, or `Blocked`.

Use `Pass` only when there are no P0-P2 findings, all applicable verified checks
passed, documentation aligns, test intent and semantic coverage are adequate,
and the applicable Definition of Done is satisfied. Never fix findings silently
or weaken a gate to obtain a pass.
