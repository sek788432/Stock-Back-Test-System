# Definition of Done

A change is **done** when every applicable item below is true. Not "I think it works" — true.

This checklist is the gate between "I made some edits" and "this can ship". It exists because:

- Humans rush. AI agents rush more. Both will declare a task complete before it actually is.
- CI catches most things, but not all (intent, performance, design, docs).
- Reviewers waste time when "done" PRs aren't actually done.

Copy the checklist into your PR description and tick boxes as you verify each item. If something is N/A, write **N/A — \<reason\>** instead of leaving it blank.

---

## Universal (every PR)

- [ ] **Scope**: PR addresses a single concern. Unrelated drive-by changes are out.
- [ ] **Branch**: branch name matches `feature|fix|docs|refactor|chore|perf/<short-name>`.
- [ ] **Commits**: messages are Conventional Commits (`feat:`, `fix:`, …).
- [ ] **Linked issue / ADR**: PR references the issue or ADR it addresses (or explains why none exists).
- [ ] **Self-reviewed**: I read my own diff hunk-by-hunk before requesting review.
- [ ] **No secrets**: no API keys, tokens, passwords, or private URLs in the diff (including comments).
- [ ] **No commented-out code**: dead code is deleted, not commented out.
- [ ] **No TODO without an issue**: every `TODO` / `FIXME` references `ISSUE-NNN`.

## Code (any production code change)

- [ ] **Builds clean** on at least one OS locally (`cmake --build --preset dev`).
- [ ] **All existing tests pass** (`ctest --preset dev`).
- [ ] **No new compiler warnings** on touched files.
- [ ] **No new clang-tidy warnings** on touched files (`tools/run-clang-tidy-diff.sh`).
- [ ] **clang-format clean** (`tools/run-clang-format.sh --check`).
- [ ] **Sanitizers clean**: ASan/UBSan/LSan reports are empty when running affected tests.
- [ ] **Skills compliance**: change does not violate `.cursor/skills/` rules — no banned C-style idioms, no raw `new`/`delete`, no `using namespace std;`, no exceptions across module boundaries, RAII for every resource.

## Threading / concurrency (when touching threads, mutexes, atomics, callbacks)

- [ ] **Cross-thread state is immutable or owned by exactly one thread** (Specs/01 §3).
- [ ] **TSan run** locally if I added new synchronization (`ctest --preset dev-tsan`).
- [ ] **No detached threads** (`std::jthread` + `std::stop_token` instead).
- [ ] **All locks are RAII** (`std::scoped_lock` / `std::shared_lock`, never `mutex.lock()` directly).
- [ ] **No `volatile` used as a synchronization primitive** (use `std::atomic`).

## Performance (any change in a documented hot path — engine bar loop, indicators, replay tick, data prefetch)

- [ ] **Benchmark numbers**: nanobench results before vs after included in PR description.
- [ ] **No new allocations** in hot loops (verify with allocator-counting test where present).
- [ ] **No new `std::function` or virtual call** in per-bar/per-tick paths.
- [ ] **Determinism unchanged** (Specs/07 §8): fixture run produces byte-identical `metrics.json` and `trades.json` — or fixture is intentionally refreshed and explained.

## Tests (every PR that touches code)

- [ ] **Every new public symbol has a unit test** (Specs/10 §7) — class, free function, public method, plus public Q_OBJECT methods.
- [ ] **Every modified public symbol has an updated or new test** that exercises the changed behavior.
- [ ] **Tests would actually fail if the production code were wrong** (mutation-aware). Mentally check: "if I flipped a `<` to `<=`, would my test catch it?"
- [ ] **No cheating patterns** (Specs/10 §5): no `EXPECT_TRUE(true)`, no tautologies, no empty test bodies, no mocking the unit under test, no silent `DISABLED_*` or `GTEST_SKIP()` without `ISSUE-NNN`.
- [ ] **Diff coverage**: ≥ 90% line coverage and ≥ 80% branch coverage on changed lines (CI computes; check the PR comment).
- [ ] **Mutation kill rate**: ≥ 70% on changed files (CI computes).

## Frontend (Qt) (any UI change)

- [ ] **No widget access from worker threads** — all updates via queued signals (Specs/02).
- [ ] **All user-visible strings wrapped in `tr(...)`**.
- [ ] **Keyboard shortcuts present** for new actions (Specs/02 §8).
- [ ] **Accessible name set** on new widgets (`accessibleName`).
- [ ] **Manual smoke test**: I opened the app, navigated to my changed area, exercised the new flow, and nothing crashes or visibly misbehaves.

## Data layer / DuckDB (any change in `Src/Backend/Data/`)

- [ ] **Read-only access only** to `MarketData.duckdb` — no writes from C++.
- [ ] **Schema discovery still works** for older DBs missing optional provenance columns (Specs/04 §3.1).
- [ ] **Multi-schema disambiguation handled** for symbols with rows in more than one `schemaName`.
- [ ] **Streaming preserved** — no `LIMIT 9999999` shortcuts that materialize everything.

## Strategy / engine (any change in `Src/Backend/Strategy/` or `Src/Backend/Engine/`)

- [ ] **Both rule mode and Lua mode** still produce identical trades for the reference `sma-cross-aapl` strategy (Specs/05 §8).
- [ ] **Determinism fixture refreshed** if engine semantics intentionally change; PR explains why.
- [ ] **`onInit → onBar* → onShutdown` contract preserved** (Liskov).

## Plugin or SDK change (any change in `Src/Backend/Strategy/Include/Bte/Plugin/` or `Src/Plugins/`)

- [ ] **`BTE_PLUGIN_ABI_MAJOR` bumped** if the change is a breaking ABI change.
- [ ] **Sample plugin still builds** in CI on all three OSes.
- [ ] **Trust prompt still fires** on first load of an unknown hash.

## Docs (any spec, ADR, README, or skill change)

- [ ] **Cross-references updated**: if I renumbered a section, every doc that links to it is fixed.
- [ ] **Examples still parse**: any code in code fences is plausible; if it's a config (JSON, YAML), it parses.
- [ ] **No broken links**: relative paths resolve, anchors exist.
- [ ] **Index updated**: `Docs/Specs/README.md`, `Docs/Decisions/README.md`, etc.

## CI / tooling (any change in `.github/`, `Cmake/`, `tools/`, `vcpkg.json`, `requirements.txt`)

- [ ] **One platform at a time**: I ran the full PR pipeline at least once on a real branch (or via `gh workflow run`).
- [ ] **Full-tree standards clean**: `checkProjectStandards.py --full-tree` reports zero violations for the commit.
- [ ] **Cache keys updated** if dependency versions changed.
- [ ] **Backwards compatible**: old contributor branches don't suddenly fail to configure.

## Release (a release-cutting PR)

- [ ] **`Docs/Governance/CHANGELOG.md`** updated with the new version section.
- [ ] **Version number** bumped consistently in CMake, `version.h`, and any user-visible "About" string.
- [ ] **All ADRs touched in this version reference the version**.
- [ ] **Release manifest** template fields filled (Specs/09 §4).
- [ ] **Plugin SDK ABI version** updated if applicable.

---

## When you can't tick everything

Three legitimate paths forward:

1. **Narrow scope**: split the PR. Land what's done; leave the rest for a follow-up PR.
2. **Mark N/A with reason**: e.g. "N/A — change is docs-only, no tests required."
3. **Ask in PR description**: "I couldn't get the TSan job to run locally on macOS arm64; please verify on Linux." A reviewer can take it from there.

What's **not** legitimate: ticking a box you didn't actually verify, or quietly skipping a section. The point of this list is the conversation you have with yourself before claiming done. Skipping that conversation defeats the purpose.

---

## Why this matters

If you ship a PR where this list is genuinely true, the change works, fits the system, has a clear maintenance story, and won't surprise anyone in a month. If you ship before the list is true, every reviewer becomes the safety net — which doesn't scale, even with four to eight people.
