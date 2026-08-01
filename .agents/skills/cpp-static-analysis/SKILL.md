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

Static analysis runs locally (pre-commit), in CI (G7 of Docs/Specs/10), and on demand. The same configs are used everywhere — there is no "CI is stricter than my machine".

## Tool stack

| Tool | What it catches | Required? |
|---|---|---|
| `clang-format` | formatting drift | yes (G8) |
| `clang-tidy` | bug-pattern lints, modernize-* checks, naming | yes (G7) |
| `clang static analyzer` (`scan-build`) | data-flow bugs (null deref, use-after-free, leaks) | yes, weekly + pre-release |
| `cppcheck` | extra coverage of patterns clang-tidy misses | yes (G7) |
| `include-what-you-use` (IWYU) | unused / missing includes | yes (G7) |
| `clang -fsanitize=address,undefined,leak` (dev preset) | runtime bugs caught at test-time | yes (G2 / G6 prerequisite) |
| `clang -fsanitize=thread` (dev-tsan preset) | data races | yes, before merging concurrency-touching PRs |

All of these are wired into the `dev` and `dev-tsan` CMake presets and `Tests/` runner. You should never have to install them by hand outside the documented setup.

## Files that own the configuration

| Path | Owns |
|---|---|
| `.clang-format` | format rules (column width, brace style, spacing) |
| `.clang-tidy` | lint checks enabled + per-check options (naming case, etc.) |
| `Tests/sanitizer-suppressions.txt` | rare false-positive suppressions (CODEOWNER review required) |
| `Cmake/Sanitizers.cmake` | which sanitizers each preset enables |
| `tools/iwyu.imp` | IWYU mapping file (Qt, std, third-party) |
| `cppcheck.suppressions` | cppcheck false-positive suppressions |

If you change one of these files, the PR description must explain why, and it requires CODEOWNER review.

## Running locally

```bash
# 1. Configure dev preset (once per machine, after submodule update)
cmake --preset dev

# 2. Format + tidy + cppcheck on changed files (pre-commit does this on `git push`)
./tools/run-clang-format.sh
./tools/run-clang-tidy-diff.sh         # only changed files
./tools/run-cppcheck.sh

# 3. Build and run tests with sanitizers
cmake --build --preset dev
ctest --preset dev

# 4. Run TSan on threading-touching changes
cmake --preset dev-tsan
cmake --build --preset dev-tsan
ctest --preset dev-tsan

# 5. Run scan-build on demand (slower; weekly in CI)
./tools/run-scan-build.sh

# 6. Check includes
./tools/run-iwyu.sh
```

These scripts read the same `compile_commands.json` CMake produces. If a tool can't find your file, run `cmake --build --preset dev` first to refresh the database.

## What "zero new warnings" means (G7)

CI compares analyzer output on `main` to analyzer output on the PR branch. **Net new warnings on touched files block merge.**

You may inherit existing warnings; you don't add new ones. Existing warnings show up in a weekly debt report — fix opportunistically.

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
3. If the fix changes semantics or you disagree, suppress with **per-line** `// NOLINT(check-name): reason` and explain in the PR. Never blanket-disable a check in `.clang-tidy` without CODEOWNER approval.

### scan-build / clang analyzer

Outputs an HTML report under `Output/scan-build/` (or your configured binary dir). Open `index.html`. Each report walks you through a path: `here we set p = nullptr; here we deref p`. Trust the path — the analyzer is good. If you think it's wrong, write a comment that proves it can't happen, and add the file/line to suppressions with a CODEOWNER review.

### Sanitizers

When ASan/UBSan/LSan/TSan fire during `ctest`, the test fails with a stack trace. Read top-down:

1. The first line tells you the bug class (`heap-use-after-free`, `data race`, `signed integer overflow`).
2. The first stack frame is where the bug *manifested*.
3. ASan also prints **previously freed at** / **previously allocated at** — the bug is usually at one of those sites, not the access site.
4. TSan prints two stacks: thread A and thread B. The bug is the missing synchronization between them.

**Never** suppress a sanitizer report without a written explanation and a CODEOWNER approval. They almost never have false positives in well-formed C++.

### IWYU

Output looks like:

```
Foo.cpp should add these lines:
#include <cstdint>     // for int64_t

Foo.cpp should remove these lines:
- #include <vector>    // lines 3-3
```

Apply the suggestions unless IWYU is wrong about a private header — in which case add a mapping to `tools/iwyu.imp`.

## clang-tidy check selection (this repo)

The `.clang-tidy` enables these check categories with naming/casing rules matching the project's `lowerCamelCase` / `UpperCamelCase` style (see `cpp-modern-style` skill):

```yaml
Checks: >
  -*,
  bugprone-*,
  cert-*,
  clang-analyzer-*,
  concurrency-*,
  cppcoreguidelines-*,
  modernize-*,
  performance-*,
  portability-*,
  readability-*,
  -modernize-use-trailing-return-type,        # too noisy on this codebase
  -readability-named-parameter,               # gtest fixtures need unnamed params
  -cppcoreguidelines-pro-bounds-pointer-arithmetic,  # span/iterator interop

CheckOptions:
  - { key: readability-identifier-naming.NamespaceCase,           value: camelBack }
  - { key: readability-identifier-naming.ClassCase,               value: CamelCase }
  - { key: readability-identifier-naming.StructCase,              value: CamelCase }
  - { key: readability-identifier-naming.EnumCase,                value: CamelCase }
  - { key: readability-identifier-naming.EnumConstantCase,        value: camelBack }
  - { key: readability-identifier-naming.FunctionCase,            value: camelBack }
  - { key: readability-identifier-naming.MethodCase,              value: camelBack }
  - { key: readability-identifier-naming.VariableCase,            value: camelBack }
  - { key: readability-identifier-naming.PrivateMemberSuffix,     value: _ }
  - { key: readability-identifier-naming.GlobalConstantCase,      value: camelBack }
  - { key: readability-identifier-naming.MacroDefinitionCase,     value: UPPER_CASE }
```

Adjust this only with a written reason and a CODEOWNER review.

## When you disagree with a tool

Static analysis is opinionated. When you genuinely think a finding is wrong:

1. **First, prove it locally.** Write a test that demonstrates the code is correct under all inputs the analyzer worried about. If you can't, the analyzer is right.
2. Suppress at the smallest scope (per-line `// NOLINT(...)`, not per-file or per-project).
3. Comment why, including the test you wrote.
4. If you needed to suppress more than 2-3 lines, the design is probably wrong — refactor instead.

## Pre-merge checklist (lift this into your PR description)

Before requesting review, confirm:

- [ ] `clang-format` clean on touched files (`./tools/run-clang-format.sh --check`)
- [ ] `clang-tidy` zero new warnings on touched files (`./tools/run-clang-tidy-diff.sh`)
- [ ] `cppcheck` zero new warnings (`./tools/run-cppcheck.sh`)
- [ ] `ctest --preset dev` passes (ASan/UBSan/LSan clean)
- [ ] If you touched threading code: `ctest --preset dev-tsan` passes
- [ ] If you touched headers: `./tools/run-iwyu.sh` clean or fixed
- [ ] All `// NOLINT` and suppression entries justified in PR description

If pre-commit passed locally and the diff is small, this is usually 30 seconds to verify. The CI does it again, so anything you forget is caught — but failing in CI is slower than checking locally.

## Working with sanitizer false positives

If a sanitizer flags external (third-party / system) code:

- Add a narrow suppression to `Tests/sanitizer-suppressions.txt`:
  ```
  leak:libduckdb.so
  race:dlopen
  ```
- Add an `until: YYYY-MM-DD` (or `never` for system code) in a comment.
- Open an issue if the third party has an upstream bug; reference it.
- Get CODEOWNER review on the suppression file.

Never suppress on **our** code without first writing the test that proves the code is correct.

## Summary

The static-analysis stack is your fastest feedback loop: it runs locally, it runs in CI, and it tells you about real bugs before tests even start. Treat its output as a code review from a tireless senior who's read every C++ defect database since 1985 — sometimes wrong, usually right, always worth answering.
