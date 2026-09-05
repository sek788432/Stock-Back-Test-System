# Changelog

All notable changes to this project are documented here. Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Current compatibility surfaces include the application, Strategy API, result
schema, and data manifest. Git history preserves superseded experiments; this
living changelog describes only the maintained product direction.

## [Unreleased]

### Added
- Project specs (`Docs/Specs/`) describing the C++/Qt desktop backtester architecture, modules, and CI policy.
- AI-agent skills (`.agents/skills/`) for modern C++, thread safety, performance, OOP/design, static analysis, and shared engineering workflows.
- Collaboration docs grouped under `Docs/Governance/` (`AGENTS.md`, `CONTRIBUTING.md`, `CHANGELOG.md`) plus `Docs/Onboarding.md`, `Docs/DefinitionOfDone.md`, `Docs/ReviewPlaybook.md`, and `Docs/ReleaseProcess.md`. The Apache-2.0 `LICENSE` and thin `AGENTS.md`/`CLAUDE.md` adapters remain at the repository root.
- Restructured: `Specs/` moved to `Docs/Specs/`; root `README.md` slimmed to point at module READMEs; long DataFetcher content moved to `DataFetcher/README.md`.
- Living important-decisions record and dependency inventory under `Docs/Decisions/`.
- Bug and feature issue templates plus a structured PR template.

### Changed
- Reconciled the canonical specs around a project-owned C++ engine, typed
  Selectable Conditions, a trusted isolated Python worker, immutable Release
  Snapshots, fixed-point accounting, and transactional `.bteresult` files.
- Distinguished implemented, planned, blocked, and non-normative behavior across
  product, CI, and release documentation; the protected StockScreening proposal
  subtree remains non-canonical pending separate review.
- Distinguished Paced Backtest engine execution from K-line Replay presentation
  so result playback cannot be mistaken for a second execution engine.
- Replaced numeric specification filenames with descriptive names so the
  canonical set can evolve without artificial ordering or renumbering.
- Defined separate Strategy and Result libraries, the five-condition responsive
  editor boundary, managed Python worker/runtime limits, immutable data-segment
  identity, durable-result compatibility, and release packaging contracts.

### Fixed
- (none)

### Removed
- Native-plugin and AI-assisted authoring specifications; neither is an
  accepted product capability.
- Obsolete K-line roadmap and organizational ownership guidance.

### Strategy API
- Not yet declared. The first implemented Python strategy contract will declare
  its version independently from the application.

---

<!--
Release sections are added below by the release manager when a tag is cut.
Format:

## [0.x.0] - YYYY-MM-DD

### Added
- ...

### Changed
- ...

### Fixed
- ...

### Removed
- ...

### Strategy API
- unchanged
- OR bumped to N — link the applicable living important-decision entry
-->
