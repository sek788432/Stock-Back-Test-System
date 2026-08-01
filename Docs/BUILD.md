# Building the C++ workspace

This repository’s C++ code lives under `Src/` and is built with **CMake 3.24+**. The first landed module is **Backend Core** (`bte::core`), starting with `Bar` ([Specs/03BackendCore.md](Specs/03BackendCore.md) §3). Qt, vcpkg-heavy dependencies, and the full app targets will wire in later per [Specs/09BuildDistributionLauncher.md](Specs/09BuildDistributionLauncher.md).

## Prerequisites

- **CMake** 3.24 or newer
- **Make** or **Ninja** — presets use **Unix Makefiles** by default so Xcode Command Line Tools are enough; install Ninja (`brew install ninja`) if you prefer it.
- A **C++20** compiler (Apple Clang, upstream Clang, or GCC 10+)

Optional:

- **Git** (for FetchContent to download Google Test on first configure)
- **Qt 6.8+** (Core, Widgets, Test) when configuring with `BTE_BUILD_QT_APP=ON`

## One-liner: `RunTest.sh`

From the repo root (configure + build + unit tests):

```bash
./RunTest.sh              # default: ctest output on failure (preset)
./RunTest.sh --verbose    # or -v — full ctest verbose output
```

`RunTest.sh` is tracked in git. Other `*.sh` files remain ignored unless you force-add them (see `.gitignore`).

## Quick start (macOS / Linux)

```bash
# From the repository root
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

This configures `Output/dev`, compiles the `bte_core` static library and the `bte_core_tests` executable, then runs unit tests.

### Sanitizers (Clang / GCC)

Use the `dev-sanitize` preset to turn on AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake --preset dev-sanitize
cmake --build --preset dev-sanitize
ctest --preset dev-sanitize
```

MSVC sanitizers are not enabled by this preset; use Visual Studio’s `/fsanitize` options if you need them on Windows.

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
| `BTE_BUILD_QT_APP` | `OFF`   | Build the optional Qt desktop app shell and frontend smoke tests |

## Optional Qt app shell

The repository still builds backend core by default. To build the initial Qt app
shell, install Qt 6.8+ and configure with:

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
| `CMakePresets.json`        | `dev`, `dev-sanitize`, `release`                  |
| `Output/<preset>/`        | CMake binary directory (gitignored; e.g. `Output/dev`) |
| `CMake/CompilerWarnings.cmake` | Shared warning flags                            |
| `CMake/Sanitizers.cmake`   | ASan/UBSan when `BTE_SANITIZERS=ON`               |
| `Src/Backend/Core/Include/Bte/Core/` | Public headers (e.g. `Bar.h`)                       |
| `Src/Backend/Core/Private/`           | Implementation `.cpp` files for Core              |
| `Tests/`                   | Google Test sources (`UnitTest_<Thing>.cpp`; see Docs/Specs/03 §1) |

## Turning tests off

Release-style builds disable tests in the `release` preset (`BTE_BUILD_TESTS=OFF`). You can also pass `-DBTE_BUILD_TESTS=OFF` when invoking CMake manually.
