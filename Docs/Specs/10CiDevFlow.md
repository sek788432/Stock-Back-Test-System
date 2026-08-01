# 10 — CI / Dev Flow
# Tmp Note : Integration Tests.. Git Action
A fully automated pull-request pipeline that enforces two non-negotiable rules:

1. **Every class and every function ships with unit tests** — no untested public symbol can land on `main`.
2. **A PR can be merged if and only if all unit tests pass *and* none of them are "cheating"** — the green check has to be earned.

This doc defines the gates, the tooling, and exactly what "cheating" means so the rule has teeth.

---

## 1. The PR contract

A pull request is **mergeable** only when **all** of the following are green:

| # | Gate | Tool | Blocks merge? |
|---|---|---|---|
| G1 | Build all matrix targets | CMake on Win/macOS/Linux | yes |
| G2 | All unit tests pass on all matrix targets | `ctest`, `pytest` | yes |
| G3 | Anti-cheat audit | `BteTestAudit` (custom) | yes |
| G4 | Symbol-coverage parity | `BteSymbolAudit` (custom) | yes |
| G5 | Line + branch coverage on changed code | `llvm-cov` / `OpenCppCoverage` + `coverage.py` | yes (≥ 90% diff line, ≥ 80% diff branch) |
| G6 | Mutation kill-rate on changed code | `mull` (LLVM) / `mutmut` (Python) | yes (≥ 70% killed, see §6) |
| G7 | Static analysis | `clang-tidy`, `cppcheck`, `ruff` | yes (no new warnings) |
| G8 | Format | `clang-format`, `ruff format` | yes (zero diff) |
| G9 | Determinism gate | engine fixture run, byte-compare | yes (per `07`) |
| G10 | Reviewer approval | GitHub branch protection | yes (1 reviewer) |
| G11 | Full-tree project standards | `Tools/CheckProjectStandards.py --full-tree` | yes (zero violations) |

GitHub branch protection on `main`:
- Require all of the above as **Required Status Checks**.
- Require linear history (rebase or squash; no merge commits).
- Require signed commits.
- Disallow force-push.
- Disallow deleting `main`.

A PR author can ship as soon as G1–G9 are green and G10 lands. **No human can override G3–G6 except through a written exemption** (see §10).

### 1.1 Full-tree standards baseline (G11)

The standards job audits every tracked UTF-8 text file at the exact commit under
test, rather than only the pull-request diff. It rejects formatting violations,
banned C++ idioms, trivial tests, non-PascalCase project paths, unit tests outside
`Tests/Unit/<Module>/`, unregistered unit-test files, and new top-level modules
without registered unit tests. Documentation may explain `TODO` and `FIXME`;
the issue-reference rule applies to source and configuration comments.

Run the same commit-level check locally with:

```bash
python3 Tools/CheckProjectStandards.py \
  --full-tree --base origin/main --head HEAD
```

The required aggregate status is `Merge gate (all required checks)`. It remains
red if either operating-system test run or the full-tree standards job fails.
See [ADR 0006](../Decisions/0006-full-tree-project-standards-gate.md).

---

## 2. Pipeline overview

```
   ┌──────────────────────────┐
   │  Local: pre-commit hook  │  fast: format + lint + changed-file tests
   └─────────────┬────────────┘
                 │  git push
                 ▼
   ┌────────────────────────────────────────────────────────────────┐
   │ GitHub Actions: pr.yml (runs on every push to a PR branch)     │
   │                                                                │
   │  job: lint-format       (ubuntu, ~30s)        — G7, G8         │
   │  job: build-and-test    (matrix Win/Mac/Linux) — G1, G2        │
   │  job: audit             (ubuntu, depends on build) — G3, G4    │
   │  job: coverage-diff     (ubuntu, depends on build) — G5        │
   │  job: mutation-diff     (ubuntu, depends on build) — G6        │
   │  job: determinism       (ubuntu, depends on build) — G9        │
   │                                                                │
   │  All jobs aggregated into a single summary status `pr/all`.    │
   └────────────────────────────────────────────────────────────────┘
                 │
                 ▼
   ┌──────────────────────────────────────────────┐
   │ Branch protection on `main` checks pr/all    │
   │ + 1 review approval → "Merge" enabled        │
   └──────────────────────────────────────────────┘
```

Total wall time goal: **< 12 minutes** for a small PR. We aggressively cache vcpkg, ccache, Qt installs, and coverage toolchain downloads.

---

## 3. Local pre-commit (fast feedback)

`pre-commit` framework, configured via `.pre-commit-config.yaml`:

```yaml
repos:
  - repo: local
    hooks:
      - id: clang-format
        name: clang-format (changed C++)
        entry: Tools/RunClangFormat.sh
        language: system
        types_or: [c++, c]
        pass_filenames: true

      - id: clang-tidy-changed
        name: clang-tidy on changed files
        entry: Tools/RunClangTidyDiff.sh
        language: system
        types_or: [c++]

      - id: ruff
        name: ruff (Python lint+format)
        entry: ruff check --fix
        language: system
        types: [python]

      - id: bte-test-audit
        name: anti-cheat audit on changed tests
        entry: Tools/BteTestAudit.py --diff
        language: system
        types_or: [c++, python]

      - id: bte-symbol-audit
        name: every public symbol has a test (changed files)
        entry: Tools/BteSymbolAudit.py --diff
        language: system
        types_or: [c++]

      - id: changed-tests-pass
        name: run tests touching changed files
        entry: Tools/RunChangedTests.sh
        language: system
        pass_filenames: true
```

Result: by the time you `git push`, the same checks the PR will run have already run locally on your delta. No "wait 12 minutes to see a stupid mistake".

`RunChangedTests.sh` uses `git diff` plus a static **module map** (which test target covers which production target) to run only relevant CTest labels. CI runs the full suite; local runs the subset.

---

## 4. Coverage policy (G5)

### 4.1 Tooling

| Language | Coverage tool | Report format |
|---|---|---|
| C++ (Linux + macOS) | `clang -fprofile-instr-generate -fcoverage-mapping` + `llvm-cov export` | LCOV |
| C++ (Windows) | `OpenCppCoverage --export_type=cobertura` | Cobertura XML |
| Python | `coverage.py` (already in pipeline-friendly form) | LCOV |

We unify everything to **LCOV** (Cobertura → LCOV via `cobertura2lcov`) so the diff tool only handles one format.

### 4.2 The "diff coverage" gate

We do **not** require absolute project-wide coverage thresholds — those punish technical debt that already exists. Instead we enforce **diff coverage** on the lines this PR adds or modifies:

| Metric | Threshold |
|---|---|
| Line coverage on changed lines | **≥ 90%** |
| Branch coverage on changed lines | **≥ 80%** |
| Files with **0%** coverage on any new public symbol | **0 allowed** (hard fail) |

Implemented with [`diff-cover`](https://pypi.org/project/diff-cover/) which takes a base ref + LCOV report and produces a pass/fail. The PR comment shows uncovered lines inline.

A `.coverageignore` file lets the team exclude generated code (Qt `.moc.cpp`, vcpkg-built sources, sample plugins). New entries to that file require justification in the PR description and reviewer signoff.

---

## 5. Anti-cheat audit (G3) — the interesting one

A test passing is a necessary but not sufficient condition for "this code works". A test passing **because the test is wrong** is worse than no test, because it gives false confidence. G3 catches the common patterns; G6 (mutation testing) catches the subtle ones.

### 5.1 What counts as cheating

A test is **cheating** when any of these apply (all checked statically by `BteTestAudit`):

#### (a) Trivially-true assertions

```cpp
EXPECT_TRUE(true);
EXPECT_EQ(1, 1);
EXPECT_NE(0, 1);
ASSERT_TRUE(!false);
EXPECT_EQ("hello", "hello");
```

Detection: AST walk of test source. Both arguments evaluate to the same compile-time literal, or the predicate is a literal.

#### (b) Tautological assertions

```cpp
EXPECT_EQ(getX(), getX());                       // calls itself on both sides
EXPECT_EQ(Constants::SIZE, Constants::SIZE);     // same symbol both sides
EXPECT_TRUE(myObj == myObj);                     // identity, not behavior
```

Detection: textual equality of LHS and RHS expressions (after canonicalization), restricted to assertion macros.

#### (c) Empty / no-assert tests

```cpp
TEST(Foo, doesNothing) { }                       // no body
TEST(Foo, justCallsIt) { foo(); }                // no assertion macro
TEST(Foo, justComment)  { /* TODO */ }
```

Detection: count `EXPECT_*` / `ASSERT_*` macros + fixture lifecycle hooks; require ≥ 1 per `TEST*` block. (Allowed exception: tests whose only assertion is `EXPECT_DEATH`, `EXPECT_NO_THROW(stmt)` followed by a comment marking them as smoke tests with a tag — see §5.4.)

#### (d) Production code never referenced

```cpp
// in UnitTest_Bar.cpp — but never names anything from Bar.h
TEST(BarTest, somethingElse) {
    int x = 1 + 1;
    EXPECT_EQ(x, 2);
}
```

Detection: cross-reference symbols used in the test body against the public header it claims to test (file naming convention enforces association: `UnitTest_Bar.cpp` ↔ `Bar.h`). Fails if zero overlap.

#### (e) Mocking what you're testing

```cpp
TEST(SmaTest, value) {
    MockSma mock;
    EXPECT_CALL(mock, value()).WillOnce(Return(42));
    EXPECT_EQ(mock.value(), 42);                 // tests the mock, not Sma
}
```

Detection: heuristic — if the class under test appears as `Mock<ClassName>`, `<ClassName>Mock`, `Fake<ClassName>` and the body never instantiates the real `<ClassName>`, fail.

#### (f) Skip-by-default and silent disables

```cpp
TEST(Foo, real) {
    GTEST_SKIP() << "flaky";                     // banned without ISSUE-### in message
    // ...
}

TEST(Foo, DISABLED_real) {                       // banned without ISSUE-###
    // ...
}

TEST(Foo, real) {
    if (std::getenv("CI")) return;               // hidden skip
    EXPECT_EQ(real(), 42);
}
```

Detection:
- Tests prefixed `DISABLED_` must have a `// ISSUE-NNN: ...` comment within 3 lines.
- `GTEST_SKIP()` calls require an issue-id in their message.
- Early `return` / `if (false) return` / `if (env) return` patterns inside `TEST*` blocks are flagged.

#### (g) Loop-of-zero / dead test loops

```cpp
TEST(Foo, perBar) {
    for (int i = 0; i < bars.size(); ++i) {      // bars empty — no iteration!
        EXPECT_EQ(bars[i].open, 100);
    }
}
```

Detection: `BteTestAudit` instruments tests with a tiny preprocessor (`BTE_TEST_LOOP`) that counts iterations of the macro-marked loops; a CI post-processing step fails the PR if any marked loop ran zero times in the test run. (Authors must use the macro for any loop containing the only assertions in a test.)

### 5.2 The audit tool: `BteTestAudit`

A small Python script using **libclang** (so we get a real AST, not regex theater):

```
Tools/BteTestAudit.py
  --src-roots Tests/  Src/Backend/  Src/Frontend/
  --header-globs '**/Include/**/*.h'
  --test-globs   '**/Tests/**/*.cpp' '**/Tests/**/*.py'
  --diff             # only audit files changed vs origin/main
  --report json|tap|text
```

Pipeline:

1. Parse each test TU with libclang's compilation database (the same `compile_commands.json` CMake exports).
2. Walk the AST. For each `CallExpr` whose callee matches `EXPECT_*` / `ASSERT_*`, record `(macro, lhsTokens, rhsTokens, location)`.
3. Apply rules (a)–(g) per §5.1.
4. Emit `audit.json` with `{ file, line, ruleId, severity, message }` per finding.
5. Exit 1 if any `severity=block`.

`audit.json` is uploaded as a CI artifact and rendered as inline PR review comments by a small GitHub Action wrapper.

### 5.3 Python tests (data pipeline)

`pytest` tests in `DataFetcher/Tests/` are audited by the same tool with rule subset (`(a)`, `(b)`, `(c)`, `(f)`):

```python
def test_loads():
    assert True                    # (a) blocked
    assert 1 == 1                  # (a) blocked
    pass                           # (c) blocked

def test_skipped():
    pytest.skip("flaky")           # (f) blocked unless ISSUE-### in message

@pytest.mark.skip
def test_disabled():               # (f) blocked unless ISSUE-### justifies
    ...
```

### 5.4 Allowed escape hatches (rare, explicit)

For genuine cases (smoke tests, compile-only tests, death tests), authors annotate:

```cpp
// BTE-AUDIT: smoke
TEST(BarTest, constructorRunsWithoutCrash) {
    Bar b;
    (void)b;
}

// BTE-AUDIT: skip-justified ISSUE-127
TEST(Foo, DISABLED_flakyOnArm) { /* ... */ }
```

The audit honors these annotations only if:
- The annotation is on the line immediately above the `TEST*` macro.
- Smoke tests are tagged in CMake with `LABEL "smoke"` and constitute < 5% of the test count per module (otherwise the suite is mostly smoke and the rule is being abused — separate "smoke ratio" check fires).
- Skips reference a real issue id matching `ISSUE-\d+` that resolves to an open ticket (validated by an Action that hits the GitHub Issues API).

---

## 6. Mutation testing (G6)

Static rules can't catch *semantic* cheating. Mutation testing can.

### 6.1 What it does

Take the production code, mutate it (e.g. replace `+` with `-`, `<` with `<=`, delete a line), re-run the affected tests. If a test still passes after the mutation, the mutant **survived** — meaning the test couldn't tell good code from broken code. Surviving mutants are evidence of weak tests.

We measure **kill rate** = `mutantsKilled / mutantsLive`.

### 6.2 Tooling

| Language | Tool |
|---|---|
| C++ | [`mull`](https://github.com/mull-project/mull) (LLVM-based) |
| Python | [`mutmut`](https://github.com/boxed/mutmut) |

Both are well-maintained and fit our build. `mull` integrates with `clang -fpass-plugin=mull-ir-frontend.so`.

### 6.3 Diff scope

Running mutation on the whole codebase is too slow. We run it **only on changed files**, with mutant operators limited to:

- Arithmetic (`+`/`-`, `*`/`/`)
- Comparison (`<`/`<=`, `>`/`>=`, `==`/`!=`)
- Boolean (`&&`/`||`)
- Constant replacement (`true`/`false`, integer ±1)
- Statement deletion (skip a line that has side effects)

Limit per PR: 200 mutants. If the diff is bigger, we sample uniformly.

### 6.4 Threshold

- **Diff kill rate ≥ 70%** to pass G6.
- Below 70%: PR fails with a list of surviving mutants and the source lines they hit. The author has to add tests until the kill rate clears.

70% is a starting point. We tune on real PRs over the first month and document the chosen number in this spec.

### 6.5 Output

Every mutation run produces `mutation-report.html` linked from the PR check, plus a `mutation-report.json` artifact. Surviving mutants appear inline as PR review comments suggesting "this line can be changed to X without any test failing — please add a test that catches it".

---

## 7. Symbol-coverage parity (G4)

> *"Every class and every function needs full unit tests."*

We make this literal:

For each public C++ header in `Src/**/Include/**/*.h`:
1. Extract every declared:
   - free function (in a `bte::` namespace),
   - class / struct (excluding pure data POD with no member functions),
   - public member function of a class,
   - public method of a `Q_OBJECT` class.
2. Compute the set `S_decl`.
3. Walk all test source files; collect every symbol the tests reference (`(`-followed identifier matching a name in `S_decl`).
4. Compute the set `S_tested`.
5. **Fail** if `S_decl \ S_tested` is non-empty.

Tool: `Tools/BteSymbolAudit.py` — also libclang-based, shares parsing with `BteTestAudit`.

For Python (`DataFetcher/`), the same rule applies: every public name (no leading underscore) in a public module must be referenced by at least one test in `DataFetcher/Tests/`.

### 7.1 Exemptions

A short YAML file `Tools/SymbolAuditExemptions.yaml`:

```yaml
exemptions:
  - symbol: bte::core::log::main
    reason: "trivial getter to spdlog singleton; covered by integration logs"
    until: 2027-01-01
  - file: Src/App/main.cpp
    reason: "Qt entry point; covered by smoke tests"
    until: never
```

Every entry must have a `reason` and an `until` date or `never`. CI fails if `until` is past today. This forces the team to revisit exemptions instead of letting them rot.

The exemption file requires a separate `CODEOWNER` review on changes (different from a normal PR reviewer), so a single dev can't quietly carve out their function.

---

## 8. Determinism gate (G9)

This is the safety net for engine changes (`07` already promises bit-identical output). On every PR:

1. Run the engine on a pinned fixture: `Tests/Determinism/fixtures/aapl-sma-cross.json`.
2. Diff the resulting `metrics.json` and `trades.json` against the committed reference outputs.
3. **Any byte-diff fails the gate**, even if other tests pass.
4. To intentionally change engine semantics: regenerate references with `make refresh-determinism-fixtures`, commit the diff, and explain it in the PR description. CODEOWNERS for `Tests/Determinism/` includes the engine maintainer, so this can't slip through.

---

## 9. Repo layout for tests

Mirrors the source tree so the symbol-audit can pair `Foo.h` ↔ `UnitTest_Foo.cpp` automatically:

```
Src/Backend/Core/
├── Include/Bte/Core/Bar.h
└── (module .cpp files alongside Include/, e.g. under `Private/`, when not header-only)
```

For header-only types (e.g. `Bar`), there may be no separate `.cpp` until the module grows.

```
Tests/Unit/Core/
├── UnitTest_Bar.cpp           # auto-paired with Bar.h
├── UnitTest_Portfolio.cpp
├── UnitTest_Result.cpp
└── CMakeLists.txt
```

Every CMake test target uses `gtest_discover_tests` so adding a TEST is a one-line change with no manifest edits.

---

## 10. Failure recovery & overrides

When you genuinely need to ship without a gate:

| Gate | Exemption mechanism | Approver |
|---|---|---|
| G3 anti-cheat | `BTE-AUDIT:` annotation per §5.4 | normal reviewer |
| G4 symbol parity | `SymbolAuditExemptions.yaml` entry with `until:` date | CODEOWNER |
| G5 coverage | line-level `// LCOV_EXCL_LINE` comment + reason in PR description | CODEOWNER for that file |
| G6 mutation | `MutationExemptions.yaml` entry with `until:` date and surviving mutant ids | CODEOWNER |
| G7 lint | `// NOLINT(check-name): reason` per line | normal reviewer |
| G9 determinism | regenerate fixture + describe semantic change | engine CODEOWNER |

There is **no** "I'll fix it later" override on G1, G2, G8. Build and existing tests must pass.

Every exemption shows up in a weekly auto-generated report (`Tests/ExemptionsReport.md`) committed by a scheduled Action so the team can pay down debt deliberately.

---

## 11. Branch model

- `main` is always green and always shippable.
- All work happens on PR branches (`feature/...`, `fix/...`).
- PRs target `main`, are squash-merged, with a Conventional-Commits message.
- Release tags (`v*.*.*`) are signed annotated tags on `main`.
- Hotfixes branch from the release tag, get the same PR gates, then forward-merge to `main`.

Auto-merge: GitHub's native "Auto-merge" is enabled for the repo. As soon as all required checks go green and a review is approved, the PR merges itself. No human waiting.

---

## 12. Onboarding checklist (for a new contributor)

```bash
git clone <repo>
cd Stock-Back-Test-System
git submodule update --init                      # vcpkg, etc.
./Tools/SetupDev.sh                             # installs pre-commit, hooks, clang-format
cmake --preset dev
cmake --build --preset dev
ctest --preset dev                                # all tests run locally first
```

`Tools/SetupDev.sh` is idempotent and prints a summary of what it installed.

A new dev cannot land a PR until they:
- Have signed commits configured.
- Pass the pre-commit on a "hello-world" PR (verifies their environment).
- Read this spec (no enforcement; it's just polite).

---

## 13. Summary in one sentence

> **A PR merges only after every changed line is covered, every changed function has a test that names it, every test is shown to actually fail when the code breaks (mutation), and no test was caught using the standard cheating patterns — across Windows, macOS, and Linux, with zero human override on the build / test gates.**

If a PR is green, the change is real.
