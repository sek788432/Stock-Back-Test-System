# Stock-Back-Test-System

A cross-platform C++ desktop application for backtesting and replaying stock trading strategies, with a Python pipeline that ingests Databento OHLCV bars into DuckDB.

> **Current status:** the repository implements CSV-backed K-line playback,
> legacy replay summaries, and a deliberately limited starter Backtest page. The
> starter engine submits one fixed-quantity market buy on the first bar, evaluates
> it at the next actual bar open, and shows the final open-position mark. Strategy
> authoring, the complete order/accounting model, the managed Python worker,
> immutable release snapshots, and `.bteresult` remain planned. See
> [`Docs/Specs/11StockScreenerKLineProduct.md`](Docs/Specs/11StockScreenerKLineProduct.md)
> for exact capability status.

> **Engine authority:** the accepted trading engine is project-owned C++.
> Selectable Conditions and the managed Python worker may submit commands to it;
> neither Python nor K-line Replay is a second execution engine.

> **AI agents:** read [`AGENTS.md`](AGENTS.md) before touching anything in this repo. It's mandatory.

## Where to start

| You are… | Start with |
|---|---|
| AI coding agent | [`AGENTS.md`](AGENTS.md) → [`Docs/Governance/AGENTS.md`](Docs/Governance/AGENTS.md) |
| Human contributor | [`Docs/Governance/CONTRIBUTING.md`](Docs/Governance/CONTRIBUTING.md) → [`Docs/Onboarding.md`](Docs/Onboarding.md) |
| Reading the design | [`Docs/Specs/`](Docs/Specs/README.md) |
| Building the C++ tree | [`Docs/BUILD.md`](Docs/BUILD.md) (`./RunTest.sh` builds and runs unit tests) |
| Reviewing a PR | [`Docs/ReviewPlaybook.md`](Docs/ReviewPlaybook.md) |
| Cutting a release | [`Docs/ReleaseProcess.md`](Docs/ReleaseProcess.md) |
| Looking for license / changelog | [`LICENSE`](LICENSE) / [`Docs/Governance/CHANGELOG.md`](Docs/Governance/CHANGELOG.md) |

## Repository layout

| Path | What's there |
|---|---|
| [`DataFetcher/`](DataFetcher/README.md) | Python pipeline (Databento → DuckDB → CSV). |
| `StockData/` | Data files (DuckDB + extracted CSVs). |
| `Src/` | C++ backend modules, Qt bindings/frontend, and application shell. Build instructions: [`Docs/BUILD.md`](Docs/BUILD.md). |
| [`Docs/Specs/`](Docs/Specs/README.md) | System architecture and module specs. |
| [`Docs/Governance/`](Docs/Governance/) | `AGENTS.md`, `CONTRIBUTING.md`, and `CHANGELOG.md`; the Apache-2.0 project license is at repository root. |
| [`Docs/Decisions/`](Docs/Decisions/) | Architecture Decision Records. |
| [`.agents/skills/`](.agents/skills/README.md) | The canonical project skills for AI agents, including repository-specific C++ rules. |
| [`.github/`](.github/) | PR/issue templates and the implemented CI workflow. |

Module-specific docs live next to their code (e.g. [`DataFetcher/README.md`](DataFetcher/README.md)). The architectural specs live under [`Docs/Specs/`](Docs/Specs/README.md).
