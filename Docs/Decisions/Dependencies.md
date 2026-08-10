# Dependencies log

This log distinguishes dependencies present in the repository from accepted but unimplemented runtime choices. Versions must be exact before a dependency becomes part of a release profile.

| Name | Version | License | Status | Used by | Reason | ADR |
|---|---|---|---|---|---|---|
| Qt 6 (Core, Widgets, Charts, Test) | CMake minimum 6.8; CI 6.9.x | LGPL-3 / Commercial | Implemented, optional app build | Bindings, Frontend, App, tests | Cross-platform desktop UI and Qt tests | [0002](0002-cpp-and-qt-as-the-desktop-stack.md) |
| GoogleTest | 1.14.0 | BSD-3-Clause | Implemented | C++ tests | Unit-test framework | — |
| databento (Python) | 0.64.0 | Apache-2.0 | Implemented, developer-only | DataFetcher | Upstream market-data acquisition | — |
| duckdb (Python) | 1.4.4 | MIT | Implemented, developer-only | DataFetcher | Mutable ingestion and verification store; not release runtime | [0011](0011-own-the-engine-and-release-data-contract.md) |
| pandas (Python) | 2.3.3 | BSD-3-Clause | Implemented, developer-only | DataFetcher | Tabular extraction | — |
| SQLite | Pin before implementation | Public domain | Planned | Results | Transactional `.bteresult` container | [0011](0011-own-the-engine-and-release-data-contract.md) |
| CPython | Pin per immutable runtime profile | PSF-2.0 | Planned | PythonStrategyRunner | Trusted isolated Python Script Strategy runtime | [0011](0011-own-the-engine-and-release-data-contract.md) |
| NumPy, pandas, SciPy, statsmodels, scikit-learn, Matplotlib, Seaborn, PyArrow | Pin each per immutable runtime profile | Verify each release manifest | Planned | `stockbt` runtime | Curated numerical, research, ML, plotting, and columnar tools; no arbitrary pip | [0011](0011-own-the-engine-and-release-data-contract.md) |
| Lua 5.4 | — | MIT | Superseded; do not add | — | Historical ADR 0003 choice replaced by Python Script Strategy | [0011](0011-own-the-engine-and-release-data-contract.md) |
| sol2 | — | MIT | Superseded; do not add | — | Historical Lua binding choice replaced | [0011](0011-own-the-engine-and-release-data-contract.md) |

Tooling is listed only when present. The current repository implements CMake/CTest, the project-standards checker, and Python tests for that checker. Other analyzers, coverage tools, mutation runners, and sanitizer CI remain planned until their commands and required workflows exist.

## License rules

- Normally allowed: MIT, BSD-2/3-Clause, Apache-2.0, MPL-2.0, ISC, Boost, zlib, and dynamically linked LGPL.
- GPL, AGPL, SSPL, and custom non-commercial terms require an explicit team decision before use.
- Application source, market data, split metadata, user strategies, and result artifacts have separate rights. A permissive application license does not grant permission to redistribute market data.
