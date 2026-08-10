# 09 — Build, Distribution, and Launcher

> **Status:** Build and test presets are partially implemented. Installers, a managed Python runtime, snapshot packaging, signing, update delivery, and a launcher are not implemented. A public data-bearing release remains **Blocked** by redistribution rights and verified split metadata.

## Supported V1 release targets

- Windows 10/11 x64.
- macOS 13+ on x64 and arm64.
- Linux x64 with glibc 2.31 or newer.

Other platforms may build from source but are not release promises.

## Build contract

- CMake is the build authority; checked-in presets are the supported entry points.
- Dependencies must be version-pinned and reproducible.
- Production code is C++20 and must follow the repository C++ rules.
- Tests are registered with CTest.
- A release build must be reproducible from a clean checkout plus documented external data inputs.

The exact commands live in [`../BUILD.md`](../BUILD.md); this spec defines the product contract and must not duplicate command details.

## Release bundle

Each installer will contain:

- the application and required shared libraries;
- a project-managed Python runtime and pinned packages used by strategy execution;
- Python strategy templates and API metadata;
- the validated immutable Release Snapshot built from approved tracked
  `StockData/Extracted` inputs; raw mutable DuckDB state is never bundled as the
  runtime data contract;
- schemas and migrations for result storage;
- license notices for code, runtime packages, and redistributed data.

The application must not depend on the user's system Python. Runtime packages are upgraded only through an application release after compatibility and regression testing.

## Managed-data release flow

1. Build the snapshot from the approved source inputs.
2. Validate schema, calendar, price constraints, split events, and supported intervals.
3. Produce a content-addressed manifest.
4. Run engine determinism and replay compatibility tests against that manifest.
5. Sign the application and snapshot manifest.
6. Publish them as one compatible release set.

Redistribution rights and split-data provenance are release blockers, not implementation assumptions.

## Versioning and compatibility

- The application follows semantic versioning.
- Strategy API, result schema, data manifest, and future native plugin ABI have independent compatibility versions.
- A release must either read supported older result files or provide a non-destructive migration.
- Unknown newer formats fail closed with a clear diagnostic.
- Removing an accepted public behavior requires a migration note and regression tests.

## Updates

V1 may use manually downloaded signed installers. An automatic updater is deferred. Any later updater must verify signatures, allow rollback, and never silently replace a data snapshot used by a stored replay.

## Merge-blocking checks: current truth

Currently implemented checks are merge-blocking wherever the repository workflow runs them:

- Ubuntu and macOS configure/build/test jobs.
- Registered CTest tests.
- Full-tree project-standards script.

The following checks are required before release but are **not implemented yet** and must not be represented as active gates:

- Windows build and test job.
- Installer/package smoke tests on every supported target.
- Managed Python-runtime and wheel compatibility tests.
- Data snapshot validation, licensing, and manifest-signature gates.
- Deterministic cross-platform golden-result comparison.
- Coverage threshold and public-behavior test audit.
- Mutation testing, clang-tidy, cppcheck, IWYU, scan-build, sanitizer, and formatting gates.
- Native symbol/export audit.
- Code signing, notarization, installer signing, and rollback testing.

## Launcher

A separate launcher is not justified for V1. If update or runtime-repair requirements later require one, it must remain a small process that verifies signed artifacts and starts the application; it must not contain business, strategy, or backtest logic.

## Release acceptance

- Clean install, first launch, sample backtest, saved result, replay, and uninstall pass on every supported target.
- Offline execution works after installation.
- The bundle never writes into its installation directory during normal use.
- A stored result retains the exact data-manifest and engine-version identity needed for replay.
- Failures identify the missing or incompatible component and preserve user data.
