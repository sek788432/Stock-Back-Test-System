# Dependencies log

This log distinguishes dependencies present in the repository from approved
but unimplemented runtime choices. Versions must be exact before a dependency
becomes part of a release profile.

| Name | Version | License | Status | Used by | Reason | Decision |
|---|---|---|---|---|---|---|
| Qt 6 Core, Concurrent, Widgets, and Test | CMake minimum 6.8; CI 6.9.x | LGPL-3 / Commercial | Implemented, optional app build | Bindings, Frontend, App, tests | Cross-platform desktop UI, owned asynchronous runs, and Qt tests | [Desktop boundary](ImportantDecisions.md#c-and-qt-desktop-boundary) |
| Qt Charts | CMake minimum 6.8; CI 6.9.x | GPL-3.0 / Commercial | Implemented, development-only; must not ship in the Apache-2.0 release | Frontend, Qt tests | Current candlestick chart pending replacement by the accepted project-owned `QPainter` chart | [Desktop boundary](ImportantDecisions.md#c-and-qt-desktop-boundary) |
| GoogleTest | 1.14.0 (`f8d7d77c06936315286eb55f8de22cd23c188571`) | BSD-3-Clause | Implemented | C++ tests | Unit-test framework | — |
| LLVM/Clang analysis tools | 18.1.3-1ubuntu1 | Apache-2.0 WITH LLVM-exception | Implemented, CI/developer-only | Static-analysis and formatting workflows | clang-format, clang-tidy, scan-build, and sanitizer toolchains | [Quality gates](ImportantDecisions.md#layered-merge-blocking-quality-gates) |
| cppcheck | 2.13.0-2ubuntu3 | GPL-3.0-or-later | Implemented, CI/developer-only; not distributed | Static-analysis workflow | Independent C++ static analysis | [Quality gates](ImportantDecisions.md#layered-merge-blocking-quality-gates) |
| include-what-you-use | 8.21-1build2 | NCSA | Implemented, CI/developer-only | Static-analysis workflow | Missing and unused include analysis | [Quality gates](ImportantDecisions.md#layered-merge-blocking-quality-gates) |
| gcovr | 8.5 | BSD-3-Clause | Implemented, CI/developer-only | Coverage workflow | gcov coverage reports | [Quality gates](ImportantDecisions.md#layered-merge-blocking-quality-gates) |
| diff-cover | 10.0.0 | Apache-2.0 | Implemented, CI/developer-only | Coverage workflow | Changed-line coverage gate | [Quality gates](ImportantDecisions.md#layered-merge-blocking-quality-gates) |
| Jinja2 | 3.1.6 | BSD-3-Clause | Implemented, CI/developer-only | Coverage workflow | HTML templates required by gcovr 8.5 reports | [Quality gates](ImportantDecisions.md#layered-merge-blocking-quality-gates) |
| MarkupSafe | 3.0.3 | BSD-3-Clause | Implemented, CI/developer-only | Coverage workflow | Escaping runtime required by Jinja2 | [Quality gates](ImportantDecisions.md#layered-merge-blocking-quality-gates) |
| pip-tools | 7.5.2 | BSD-3-Clause | Implemented, lock-generation-only | Coverage dependency lock | Reproduce the hash-locked coverage tool dependency closure | [Quality gates](ImportantDecisions.md#layered-merge-blocking-quality-gates) |
| databento (Python) | 0.64.0 | Apache-2.0 | Implemented, developer-only | DataFetcher | Upstream market-data acquisition | — |
| duckdb (Python) | 1.4.4 | MIT | Implemented, developer-only | DataFetcher | Mutable ingestion and verification store; not release runtime | [Engine authority](ImportantDecisions.md#engine-and-release-data-authority) |
| pandas (Python) | 2.3.3 | BSD-3-Clause | Implemented, developer-only | DataFetcher | Tabular extraction | — |
| SQLite | 3.53.4, exact installed-package discovery | Public domain | Approved implementation pin | Results | Transactional `.bteresult` container | [Canonical result storage](ImportantDecisions.md#canonical-result-storage-and-lifecycle) |
| CPython | Pin per immutable runtime profile | PSF-2.0 | Planned | PythonStrategyRunner | Trusted isolated Python Script Strategy runtime | [Engine authority](ImportantDecisions.md#engine-and-release-data-authority) |
| NumPy and pandas | Pin each per immutable runtime profile | BSD-3-Clause | Planned | `stockbt` runtime | Approved numerical and tabular packages; no arbitrary pip or broader research stack | [Engine authority](ImportantDecisions.md#engine-and-release-data-authority) |

Tooling is listed only when present. Semantic test-intent auditing, mutation
testing, Clazy, CodeQL, and a custom Clang AST policy plugin remain planned
until their exact commands and required workflows exist. The sanitizer matrix
and clang-format 18 full-tree check are implemented and merge-blocking.

## License rules

- Normally allowed: MIT, BSD-2/3-Clause, Apache-2.0, MPL-2.0, ISC, Boost, zlib,
  and dynamically linked LGPL.
- GPL, AGPL, SSPL, and custom non-commercial terms require an explicit team
  decision before use.
- Application source, market data, split metadata, user strategies, and result
  artifacts have separate rights. A permissive application license does not
  grant permission to redistribute market data.
