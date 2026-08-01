# 0010 — Enforce PascalCase paths and unit-test layout

- **Status**: Accepted
- **Date**: 2026-08-01
- **Deciders**: project maintainers
- **Supersedes**: —
- **Superseded by**: —

## Context

Project-owned paths currently mix PascalCase, snake_case, kebab-case, lowercase,
and names containing spaces. Test suites are also split between `Tests/Unit/`
and sibling directories even when every test in those siblings is a unit test.
The inconsistency makes paths harder to predict and obscures the distinction
between test level and tested module.

Some names cannot follow the project convention because external tools discover
them by exact spelling. Other names represent domain identifiers, such as stock
symbols, rather than project concepts.

## Decision

- Use PascalCase for every project-owned directory name and file stem.
- Keep exact names required by external tools or established repository
  protocols, including dot-directories, `CMakeLists.txt`, conventional root and
  governance files, numbered ADR slugs, skill package files, Python dependency
  manifests, `main.cpp`, and `UnitTest_<Thing>` test names.
- Keep domain-data filenames whose stems are external identifiers, including
  stock-symbol CSV fixtures.
- Place every unit-test suite under `Tests/Unit/<Module>/`. Keep shared fixtures
  under `Tests/Fixtures/`; future integration and system suites use their own
  top-level test-level directories.
- Enforce the path convention and unit-test placement in the full-tree project
  standards gate.

## Consequences

**Positive:**

- Contributors can derive project paths from concept names consistently.
- `Tests/` communicates test level before module ownership.
- CI prevents naming and unit-test placement drift.

**Negative:**

- Existing links, scripts, and build manifests require a one-time coordinated
  rename.
- Case-only renames need care on case-insensitive filesystems.
- The standards checker maintains a small explicit list of conventional and
  domain-data exceptions.

## Alternatives considered

1. **Apply PascalCase only to new paths.** Rejected because the mixed existing
   tree would remain hard to navigate and copy.
2. **Keep unit suites beside their source-module names directly under
   `Tests/`.** Rejected because test level remains implicit and inconsistent.
3. **Rename external-tool files too.** Rejected because GitHub, CMake, agent
   skill discovery, and other tools require exact conventional spellings.
4. **Document the rule without enforcement.** Rejected because the existing
   full-tree standards gate is the natural place to prevent regressions.

## References

- [Canonical agent playbook](../Governance/AGENTS.md)
- [CI and development-flow specification](../Specs/10CiDevFlow.md)
- [Modern C++ style skill](../../.agents/skills/cpp-modern-style/SKILL.md)
