# Copilot pull-request review instructions

Review every pull request against the repository's canonical rules in
`Docs/Governance/AGENTS.md`, the relevant `Docs/Specs/` documents, the accepted
ADRs in `Docs/Decisions/`, and `Docs/DEFINITION_OF_DONE.md`.

Prioritize concrete correctness, safety, and regression findings. In particular:

- Verify new behavior has meaningful unit tests that would fail for an incorrect
  implementation. Flag trivial assertions, tautologies, silent skips, and tests
  that only exercise mocks.
- For every new `Src/<Module>` or `Src/Backend/<Module>` tree, verify matching
  `UnitTest_*.cpp` or `UnitTest_*.py` coverage exists under `Tests/` and is
  registered with CTest through CMake.
- Reject raw `new`, `delete`, `malloc`, `free`, `using namespace std`, C-style
  casts, `NULL`, manual mutex lock/unlock, and detached threads.
- Verify public module-boundary APIs return `bte::core::Result<T, Error>` and do
  not allow exceptions to cross module boundaries.
- Verify C++ code never writes to `StockData/MarketData.duckdb`; only the Python
  data pipeline may write that database.
- Flag changes that can break deterministic engine output without an intentional
  fixture update and explanation.
- Check module dependency direction, ownership, thread safety, bounds, error
  handling, and cross-platform behavior on Linux and macOS.
- Flag every new compiler warning, formatting regression, unregistered test, or
  CI path that can succeed without running tests.

Report only actionable findings. Give each finding a severity, explain the
failure scenario, and point to the smallest relevant file and line range. Do not
treat a green CI result as proof that the implementation is correct.
