# Stock-Back-Test-System

A cross-platform C++ desktop application for backtesting and replaying stock trading strategies, with a Python pipeline that ingests Databento OHLCV bars into DuckDB.

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
| Looking for license / changelog | [`Docs/Governance/`](Docs/Governance/) |

## Repository layout

| Path | What's there |
|---|---|
| [`DataFetcher/`](DataFetcher/README.md) | Python pipeline (Databento → DuckDB → CSV). |
| `StockData/` | Data files (DuckDB + extracted CSVs). |
| `Src/` | C++ backend (starts with `Backend/Core`). Build instructions: [`Docs/BUILD.md`](Docs/BUILD.md). |
| [`Docs/Specs/`](Docs/Specs/README.md) | System architecture and module specs. |
| [`Docs/Governance/`](Docs/Governance/) | `AGENTS.md`, `CONTRIBUTING.md`, `CHANGELOG.md`, `LICENSE`. |
| [`Docs/Decisions/`](Docs/Decisions/) | Architecture Decision Records. |
| [`.agents/skills/`](.agents/skills/README.md) | The canonical project skills for AI agents, including repository-specific C++ rules. |
| [`.github/`](.github/) | PR/issue templates, CODEOWNERS. |

Module-specific docs live next to their code (e.g. [`DataFetcher/README.md`](DataFetcher/README.md)). The architectural specs live under [`Docs/Specs/`](Docs/Specs/README.md).
