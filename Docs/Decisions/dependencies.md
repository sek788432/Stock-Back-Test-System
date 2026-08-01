# Dependencies log

A rolling list of every third-party dependency the codebase pulls in, why we chose it, and which ADR (if any) records the decision.

Add a row when adopting a new dependency (per [`../Governance/AGENTS.md`](../Governance/AGENTS.md) §6 and [`../Governance/CONTRIBUTING.md`](../Governance/CONTRIBUTING.md)).

| Name | Version | License | Used by | Reason | ADR |
|---|---|---|---|---|---|
| Qt 6 (Base, Widgets, Charts, Tools) | 6.8.x | LGPL-3 / Commercial | Frontend, App, Launcher | Cross-platform desktop UI; native look on all 3 OSes | [0002](0002-cpp-and-qt-as-the-desktop-stack.md) |
| DuckDB | (pinned in `vcpkg.json`) | MIT | Backend/Data | Read OHLCV from `MarketData.duckdb`; same store the Python pipeline writes | — (existing) |
| spdlog | (pinned) | MIT | All backend modules | Structured async logging, multi-sink | — |
| fmt | (pinned) | MIT | All backend modules | `std::format` predecessor; pulled by spdlog | — |
| Lua 5.4 | 5.4.x | MIT | Backend/Strategy | Strategy scripting runtime, sandboxable | [0003](0003-hybrid-rule-and-lua-strategy-authoring.md) |
| sol2 | (pinned) | MIT | Backend/Strategy | Idiomatic C++ ↔ Lua binding | [0003](0003-hybrid-rule-and-lua-strategy-authoring.md) |
| nlohmann-json | (pinned) | MIT | Backend/Strategy, Bindings | Rule JSON parsing; release-manifest read/write | — |
| GoogleTest | (pinned) | BSD-3 | Tests | C++ test framework | — |
| nanobench | (vendored, single header) | MIT | Tests/Bench | Microbenchmarks for hot paths | — |
| moodycamel `ReaderWriterQueue` | (vendored) | BSD-style | Backend/Data | Single-producer/single-consumer ring for prefetch | — |
| pandas / lxml / databento / duckdb (Python) | per `requirements.txt` | various permissive | DataFetcher | Existing data pipeline | — (existing) |
| Matt Pocock Skills | 1.1.0 (`2ab9580`) | MIT | AI agent workflows | Shared, project-scoped engineering and productivity skills | [0008](0008-standardize-project-agent-skills.md) |

Tooling-only (not linked into shipped binaries):

| Name | Used by |
|---|---|
| clang-format, clang-tidy, clang-analyzer (scan-build) | CI gate G7, G8 |
| cppcheck | CI gate G7 |
| include-what-you-use | CI gate G7 |
| OpenCppCoverage (Windows) | CI gate G5 |
| llvm-cov (Linux/macOS) | CI gate G5 |
| `mull` | CI gate G6 |
| `mutmut` | CI gate G6 (Python) |
| `diff-cover` | CI gate G5 |
| pre-commit | local hook orchestration |
| ruff | Python lint + format |

---

## License rules (recap)

- **Allowed without further discussion**: MIT, BSD (2/3-clause), Apache-2.0, MPL-2.0, ISC, Boost, zlib, LGPL (dynamically linked).
- **Forbidden without an explicit team decision and ADR**: GPL-2.0, GPL-3.0, AGPL, SSPL, custom "non-commercial" licenses.

If a candidate dependency is borderline (e.g. has multiple licenses, is dual-licensed with weird terms), open a draft ADR and discuss before pulling it in.
