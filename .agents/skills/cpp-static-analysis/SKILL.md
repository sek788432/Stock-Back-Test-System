---
name: cpp-static-analysis
description: >-
  Run and interpret the static-analysis tool stack for this repository's C++
  code: clang-format, clang-tidy, clang static analyzer (scan-build), cppcheck,
  include-what-you-use (IWYU), and the sanitizer-instrumented dev build. Use
  when the user asks about static analysis, lint, tidy, sanitizers, scan-build,
  cppcheck, IWYU, format, warnings, .clang-tidy / .clang-format configs, or
  before opening / merging a pull request.
---

# Static Analysis

Static analysis runs locally, in CI (`Docs/Specs/CiDevFlow.md`), and on demand. The
repository does not currently install a pre-commit hook. Local and CI execution
use the same checked-in analyzer and coverage configuration.

## Tool stack

| Tool | What it catches | Required? |
|---|---|---|
| `clang-format` | formatting drift | yes, project-standards gate |
| `clang-tidy` | bug-pattern lints, modernize-* checks, naming | yes (§2) |
| `clang static analyzer` (`scan-build`) | data-flow bugs (null deref, use-after-free, leaks) | yes (§2) |
| `cppcheck` | extra coverage of patterns clang-tidy misses | yes (§2) |
| `include-what-you-use` (IWYU) | unused / missing includes | yes (§2) |
| `clang -fsanitize=address,undefined` (`dev-sanitize`) | runtime bugs caught at test-time | yes, sanitizer matrix |
| `clang -fsanitize=thread` (`dev-tsan`) | data races | yes, sanitizer matrix |

The implemented merge-blocking analyzer, format, and coverage gates are
orchestrated locally by `RunQuality.sh`. The sanitizer presets are separate CI
jobs and local CMake presets. See `Docs/Specs/CiDevFlow.md` for the
authoritative implemented-versus-planned status.

## Files that own the configuration

| Path | Owns |
|---|---|
| `.clang-format` | format rules (column width, brace style, spacing) |
| `.clang-tidy` | lint checks enabled + per-check options (naming case, etc.) |
| `CMake/Sanitizers.cmake` | which sanitizers each preset enables |
| `Tools/Iwyu.imp` | IWYU mapping file (Qt, std, third-party) |
| `Cppcheck.suppressions` | cppcheck false-positive suppressions |

If you change one of these files, the PR description must explain why and a
repository maintainer must approve the focused tooling change.

## Running locally

```bash
# Complete registered test suite, matching the CI Qt build.
./RunTest.sh

# Complete full-project static-analysis and changed-coverage workflow.
./RunQuality.sh --base origin/main --head HEAD

# Faster iteration: skips whole-tree scan-build only; the other analyzers stay full-project.
./RunQuality.sh --fast --base origin/main --head HEAD

# Local ASan/UBSan/LSan run matching one sanitizer matrix entry.
cmake --preset dev-sanitize
cmake --build --preset dev-sanitize --parallel
ctest --preset dev-sanitize --no-tests=error

# Local TSan run matching the other sanitizer matrix entry.
cmake --preset dev-tsan
cmake --build --preset dev-tsan --parallel
ctest --preset dev-tsan --no-tests=error
```

Run the complete, non-`--fast` command on the committed `HEAD` before pushing
applicable C++ work or requesting CI. The analyzer scripts read the compilation
database under `Output/analysis`, and coverage reports are generated under
`Output/CoverageReport/`. The first invocation bootstraps missing analyzer
packages and a private locked Python environment; no activation or manual
`PATH` setup is required.

## What analyzer-clean means

Every CI event and the complete default `RunQuality.sh` invocation check
clang-format and run clang-tidy, cppcheck, IWYU, and scan-build across all
project translation units. `RunQuality.sh --fast` skips only scan-build. CI
additionally runs the complete registered non-Qt test suite under both
sanitizer configurations. Any finding fails its job; there is no warning
baseline comparison or weekly debt report.

## Reading analyzer output

### clang-tidy

Each finding looks like:

```
/path/to/Foo.cpp:42:15: warning: pass 'std::string' by const reference
[modernize-pass-by-value,-warnings-as-errors]
    void doIt(std::string s) {
              ^~~~~~~~~~~
```

Read in this order:
1. The check name in brackets — Google `clang-tidy modernize-pass-by-value` if unfamiliar.
2. The fix-it underline — `clang-tidy --fix` will apply suggested fixes for many checks.
3. If the fix changes semantics or you disagree, suppress with **per-line** `// NOLINT(check-name): reason` and explain in the PR. Never blanket-disable a check in `.clang-tidy` without maintainer approval.

### scan-build / clang analyzer

Outputs HTML reports under `Output/scan-build-reports/`. Reproduce the reported
path before deciding a finding is false. Any suppression must be narrow,
justified, and maintainer-approved under governance.

### Sanitizers

When ASan/UBSan/LSan/TSan fire during `ctest`, the test fails with a stack trace. Read top-down:

1. The first line tells you the bug class (`heap-use-after-free`, `data race`, `signed integer overflow`).
2. The first stack frame is where the bug *manifested*.
3. ASan also prints **previously freed at** / **previously allocated at** — the bug is usually at one of those sites, not the access site.
4. TSan prints two stacks: thread A and thread B. The bug is the missing synchronization between them.

**Never** suppress a sanitizer report merely to make a gate pass. A future
narrow suppression requires the focused tooling change and maintainer approval
defined by governance.

### IWYU

Output looks like:

```
Foo.cpp should add these lines:
#include <cstdint>     // for int64_t

Foo.cpp should remove these lines:
- #include <vector>    // lines 3-3
```

Apply the suggestions unless IWYU is wrong about a private header — in which case add a mapping to `Tools/Iwyu.imp`.

## clang-tidy check selection

The checked-in [`.clang-tidy`](../../../.clang-tidy) file is the only authority
for enabled checks and options. Inspect it at the submitted revision rather
than copying a configuration excerpt into this skill. Adjust it only through a
focused, tested tooling change with a written reason and maintainer approval.

## When you disagree with a tool

Static analysis is opinionated. When you genuinely think a finding is wrong:

1. **First, prove it locally.** Write a test that demonstrates the code is correct under all inputs the analyzer worried about. If you can't, the analyzer is right.
2. Suppress at the smallest scope (per-line `// NOLINT(...)`, not per-file or per-project).
3. Comment why, including the test you wrote.
4. If you needed to suppress more than 2-3 lines, the design is probably wrong — refactor instead.

## Pre-merge checklist (lift this into your PR description)

Before requesting review, confirm:

- [ ] `./RunTest.sh` passes on the submitted commit
- [ ] `./RunQuality.sh --base <base> --head HEAD` passes without `--fast`
- [ ] Applicable `ctest --preset dev-sanitize` and `ctest --preset dev-tsan` pass
- [ ] All `// NOLINT` and suppression entries justified in PR description

CI repeats the checks against the submitted SHA; it is the verifier, not the
first feedback loop.

## Working with sanitizer false positives

No sanitizer-suppression file is currently wired into the build. Treat every
finding as a defect. A future suppression mechanism must be introduced as a
narrow, reviewed tooling change before any suppression can be used.

## Summary

The static-analysis stack is your fastest feedback loop: it runs locally, it runs in CI, and it tells you about real bugs before tests even start. Treat its output as a code review from a tireless senior who's read every C++ defect database since 1985 — sometimes wrong, usually right, always worth answering.
