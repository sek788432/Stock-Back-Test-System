# Dependencies log

This log distinguishes repository dependencies from locally assembled runtime
profile inputs and full release dependencies. Versions must be exact before a
dependency becomes part of a release profile.

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
| SQLite | 3.53.4, vcpkg port revision 0 at `9e593bb18ea69cc5095e012465dcd675a822ed0d` | Public domain (`blessing`) | Implemented; root manifest and exact CMake discovery | Results | Transactional `.bteresult` container | [Canonical result storage](ImportantDecisions.md#canonical-result-storage-and-lifecycle) |
| nlohmann/json | 3.12.0, vcpkg port revision 2 at `9e593bb18ea69cc5095e012465dcd675a822ed0d` | MIT | Implemented, exact vcpkg/CMake discovery | PythonStrategyRunner protocol parser | Strict JSON DOM/SAX parsing; project-owned RFC 8785 canonical serializer remains the wire/hash authority | [Python Strategy boundary](ImportantDecisions.md#python-strategy-boundary-and-runtime-profile) |
| python-build-standalone (build source/tooling) | Release `20261021`; product archive and SHA-256 are pinned in [BuildDistribution.md](../Specs/BuildDistribution.md) | MPL-2.0 build tooling; produced CPython archive retains CPython and bundled-library notices | Implemented as an optional macOS app build input; not shipped as tooling | Python runtime packaging | Reproducible prebuilt CPython profile for macOS Apple Silicon | [Python Strategy boundary](ImportantDecisions.md#python-strategy-boundary-and-runtime-profile) |
| CPython | 3.14.9, Astral `python-build-standalone` release `20261021`, `aarch64-apple-darwin` `install_only`; archive SHA-256 is in [BuildDistribution.md](../Specs/BuildDistribution.md) | PSF-2.0 plus every bundled-library notice in the exact archive | Implemented in the optional macOS Apple Silicon profile; omitted from the default developer app | PythonStrategyRunner | Fresh managed interpreter for the initial Python Strategy workflow; no system-Python fallback | [Python Strategy boundary](ImportantDecisions.md#python-strategy-boundary-and-runtime-profile) |
| NumPy | 2.5.4 CPython 3.14 macOS ARM64 wheel; SHA-256 in [BuildDistribution.md](../Specs/BuildDistribution.md) | BSD-3-Clause plus notices for bundled native components | Implemented in the optional managed runtime profile | Managed Python Strategy runtime | Approved numerical package; Python 3.14-compatible wheel selected | [Python Strategy boundary](ImportantDecisions.md#python-strategy-boundary-and-runtime-profile) |
| pandas | 3.0.6 CPython 3.14 macOS ARM64 wheel; SHA-256 in [BuildDistribution.md](../Specs/BuildDistribution.md) | BSD-3-Clause | Implemented in the optional managed runtime profile | Managed Python Strategy runtime | Approved tabular package; not exposed as market-history storage | [Python Strategy boundary](ImportantDecisions.md#python-strategy-boundary-and-runtime-profile) |
| python-dateutil | 2.9.0.post0 universal wheel; SHA-256 in [BuildDistribution.md](../Specs/BuildDistribution.md) | Apache-2.0 OR BSD-3-Clause | Implemented as a pinned transitive pandas dependency | Managed Python Strategy runtime | Exact pandas runtime dependency closure | [Python Strategy boundary](ImportantDecisions.md#python-strategy-boundary-and-runtime-profile) |
| six | 1.17.0 universal wheel; SHA-256 in [BuildDistribution.md](../Specs/BuildDistribution.md) | MIT | Implemented as a pinned transitive python-dateutil dependency | Managed Python Strategy runtime | Exact python-dateutil runtime dependency closure | [Python Strategy boundary](ImportantDecisions.md#python-strategy-boundary-and-runtime-profile) |
| tzdata | 2026.5 universal wheel; SHA-256 in [BuildDistribution.md](../Specs/BuildDistribution.md) | Apache-2.0 | Implemented in the optional profile and selected as the worker's timezone database | Managed Python Strategy runtime | Avoid host timezone-database drift for strategy date/time conversions | [Python Strategy boundary](ImportantDecisions.md#python-strategy-boundary-and-runtime-profile) |
| `stockbt` | Built from the application revision; exact source and installed-file digests are in the runtime profile manifest | Apache-2.0 | Implemented in the optional managed runtime profile | Managed Python Strategy runtime | Project-owned, chronology-safe values and narrow command API; not a third-party backtest engine | [Python Strategy boundary](ImportantDecisions.md#python-strategy-boundary-and-runtime-profile) |

Tooling is listed only when present. Semantic test-intent auditing, mutation
testing, Clazy, CodeQL, and a custom Clang AST policy plugin remain planned
until their exact commands and required workflows exist. The sanitizer matrix
and clang-format 18 full-tree check are implemented and merge-blocking.

SQLite provenance is fixed by the root [`vcpkg.json`](../../vcpkg.json).
The pinned [port manifest](https://github.com/microsoft/vcpkg/blob/9e593bb18ea69cc5095e012465dcd675a822ed0d/ports/sqlite3/vcpkg.json)
selects 3.53.4 with its existing default `json1` feature; the
[port recipe](https://github.com/microsoft/vcpkg/blob/9e593bb18ea69cc5095e012465dcd675a822ed0d/ports/sqlite3/portfile.cmake)
verifies the upstream `sqlite-autoconf-3530400.tar.gz` archive with SHA-512
`c24374e9393a943157f533f96e89e6c5743e5f5aad169d8393cff3088ca5ccbe5cc0561681ace49c349d0fe402298ee2624d319f422967247ce0792e3b3aa01e`.
That immutable registry also pins the port's host build helpers. SQLite's
public-domain dedication permits use in this Apache-2.0 application; the port
installs its copyright notice. Local setup is in [`BUILD.md`](../BUILD.md).

## License rules

- Normally allowed: MIT, BSD-2/3-Clause, Apache-2.0, MPL-2.0, ISC, Boost, zlib,
  and dynamically linked LGPL.
- GPL, AGPL, SSPL, and custom non-commercial terms require an explicit team
  decision before use.
- Application source, market data, split metadata, user strategies, and result
  artifacts have separate rights. A permissive application license does not
  grant permission to redistribute market data.
