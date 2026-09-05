# Building the C++ workspace

This repository’s C++ code lives under `Src/` and is built with **CMake 3.24+**.
Core, Data, streaming Indicators, the Selectable Strategy path, the bar-only
Replay, and the limited Engine build in the default preset. Bindings, Frontend,
and App targets are implemented behind `BTE_BUILD_QT_APP`, including the
starter and Selectable Conditions Backtest page. General strategy execution,
broker/accounting/metrics, and canonical result persistence described in
[`Specs/StrategyAuthoring.md`](Specs/StrategyAuthoring.md) and
[`Specs/EngineReplayPnL.md`](Specs/EngineReplayPnL.md) remain planned.

## Prerequisites

- **CMake** 3.24 or newer
- **Make** or **Ninja** — presets use **Unix Makefiles** by default so Xcode Command Line Tools are enough; install Ninja (`brew install ninja`) if you prefer it.
- A **C++20** compiler (Apple Clang, upstream Clang, or GCC 10+)

Optional:

- **Git** (for FetchContent to download Google Test on first configure)
- **Qt 6.8+** (Core, Concurrent, Widgets, Charts, and Test when tests are enabled) when configuring with `BTE_BUILD_QT_APP=ON`
- **Local quality bootstrap:** Homebrew on macOS, or `apt` and `sudo` access on
  Ubuntu. `RunQuality.sh` installs the CI-compatible LLVM 18 analyzer toolchain,
  the remaining analyzers, and Python 3.12, then owns a hash-locked Python
  environment under `Output/QualityVenv/` automatically.

## Root developer scripts

From the repo root, rebuild and run every registered backend and Qt test:

```bash
./RunTest.sh              # default: ctest output on failure (preset)
./RunTest.sh --verbose    # or -v — full ctest verbose output
```

Rebuild the warning-clean Qt application and launch its frontend window:

```bash
./Launch.sh
```

Both scripts use `qt-dev`, so Qt 6.8+ is required. `RunTest.sh` matches the
implemented all-tests workflow and fails if no tests are registered. Other
`*.sh` files remain ignored unless explicitly added to `.gitignore`.

## Quick start (macOS / Linux)

```bash
# From the repository root
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

This configures `Output/dev`, builds the default backend targets and registered
non-Qt tests, then runs them.

### Sanitizers (Clang / GCC)

Use the `dev-sanitize` preset to turn on AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake --preset dev-sanitize
cmake --build --preset dev-sanitize
ctest --preset dev-sanitize
```

ThreadSanitizer is a separate preset because its runtime cannot be combined
with ASan/UBSan:

```bash
cmake --preset dev-tsan
cmake --build --preset dev-tsan
ctest --preset dev-tsan
```

These presets support Clang/GNU. A checked-in MSVC sanitizer workflow is not
available.

## Manual configure (no preset)

```bash
cmake -S . -B Output -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DBTE_BUILD_TESTS=ON
cmake --build Output
cd Output && ctest --output-on-failure
```

## CMake options

| Option             | Default | Meaning                                      |
| ------------------ | ------- | -------------------------------------------- |
| `BTE_BUILD_TESTS`  | `ON`    | Fetch Google Test and build `Tests/` targets |
| `BTE_SANITIZERS`   | `OFF`   | ASan/UBSan on Clang/GNU when set to `ON`     |
| `BTE_THREAD_SANITIZER` | `OFF` | TSan on Clang/GNU; mutually exclusive with `BTE_SANITIZERS` |
| `BTE_BUILD_QT_APP` | `OFF`   | Build the optional Qt desktop app shell and frontend smoke tests |
| `BTE_COVERAGE`     | `OFF`   | Add gcov-compatible test coverage instrumentation |

The `analysis` and `coverage` presets back the merge-blocking quality jobs.
Analyzer and changed-code coverage commands are documented in
[`Specs/CiDevFlow.md`](Specs/CiDevFlow.md) §7.

Before pushing C++ work or requesting CI, run the complete local equivalents on
the committed revision:

```bash
./RunTest.sh
./RunQuality.sh --base origin/main --head HEAD
```

`RunQuality.sh` runs full-project clang-tidy, cppcheck, IWYU, and scan-build,
all coverage-instrumented registered tests, the 98%
changed-line gate, and the 90% changed-branch gate. During iteration,
`./RunQuality.sh --fast` skips scan-build; it does not replace the complete
pre-CI run. Its first invocation installs missing quality tools and creates its
private coverage environment; no activation or `PATH` setup is required.
Reports are written to `Output/CoverageReport/` and remain untracked.

After a GitHub Actions run, the coverage totals and changed-code gate results
are rendered on the workflow summary page. For annotated source details, open
the run's **Artifacts** section, download `coverage-reports`, extract it, and
open `coverage.html`. The local report is
`Output/CoverageReport/index.html`.

## Optional Qt app shell

The repository builds non-Qt backend targets by default. To build the
implemented Qt app shell and all currently registered tests, install Qt 6.8+
and configure with:

```bash
cmake --preset qt-dev
cmake --build --preset qt-dev
ctest --preset qt-dev
```

If CMake cannot find Qt, set `CMAKE_PREFIX_PATH` to your Qt install prefix before
configuring.

## Layout

| Path                         | Role                                              |
| ---------------------------- | ------------------------------------------------- |
| `CMakeLists.txt`             | Root project, optional tests, FetchContent (GTest) |
| `CMakePresets.json`        | `dev`, `qt-dev`, `dev-sanitize`, `dev-tsan`, `analysis`, `coverage`, `release` |
| `Output/<preset>/`        | CMake binary directory (gitignored; e.g. `Output/dev`) |
| `CMake/CompilerWarnings.cmake` | Shared warning flags                            |
| `CMake/Sanitizers.cmake`   | ASan/UBSan or TSan instrumentation                 |
| `Src/Backend/Core/Include/Bte/Core/` | Public headers (e.g. `Bar.h`)                       |
| `Src/Backend/Core/Private/`           | Implementation `.cpp` files for Core              |
| `Tests/`                   | Google Test sources (`UnitTest_<Thing>.cpp`; see `Docs/Specs/BackendCore.md` §2) |

## Turning tests off

Release-style builds disable tests in the `release` preset (`BTE_BUILD_TESTS=OFF`). You can also pass `-DBTE_BUILD_TESTS=OFF` when invoking CMake manually.
