# Changelog

All notable changes to this project are documented here. Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

The plugin ABI version (`BTE_PLUGIN_ABI_MAJOR`) and the Lua API version (`bte.apiVersion`) version independently — see entries below.

## [Unreleased]

### Added
- Project specs (`Docs/Specs/`) describing the C++/Qt desktop backtester architecture, modules, and CI policy.
- AI-agent skills (`.agents/skills/`) for modern C++, thread safety, performance, OOP/design, static analysis, and shared engineering workflows.
- Collaboration docs grouped under `Docs/Governance/` (`AGENTS.md`, `CONTRIBUTING.md`, `CHANGELOG.md`, `LICENSE`) plus `Docs/ONBOARDING.md`, `Docs/DEFINITION_OF_DONE.md`, `Docs/REVIEW_PLAYBOOK.md`, `Docs/RELEASE_PROCESS.md`. Repo root keeps a thin `AGENTS.md` pointer for AI tool auto-discovery.
- Restructured: `Specs/` moved to `Docs/Specs/`; root `README.md` slimmed to point at module READMEs; long DataFetcher content moved to `DataFetcher/README.md`.
- ADR framework (`Docs/Decisions/`) with the meta-ADR and three accepted decisions.
- Issue templates (bug, feature, design-rfc) and a structured PR template.

### Changed
- (none)

### Fixed
- (none)

### Removed
- (none)

### Plugin ABI
- Not yet declared. Will be `1` at first release.

### Lua API
- Not yet declared. Will be `1` at first release.

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

### Plugin ABI
- unchanged from previous release
- OR bumped to N — see ADR NNNN

### Lua API
- unchanged
- OR bumped to N — see ADR NNNN
-->
