# 09 — Build, Distribution, and the Launcher

How the whole thing is built once, packaged per OS, hosted on GitHub, and how a local user installs and switches between versions painlessly.

---

## 1. Build system

**CMake 3.24+** with **`CMakePresets.json`** and **vcpkg manifest mode** for third-party deps.

Why:
- CMake is the de-facto C++/Qt build system; Qt 6 ships first-class CMake support (`qt_add_executable`, `qt_add_translations`, `windeployqt`/`macdeployqt` targets).
- `CMakePresets.json` standardizes invocations across IDEs (CLion, VS, Qt Creator) and CI.
- vcpkg manifest (`vcpkg.json` checked into the repo) pins exact versions of DuckDB, spdlog, fmt, sol2, Lua, and friends — reproducible builds.

### 1.1 Toplevel `CMakeLists.txt` (sketch)

```cmake
cmake_minimum_required(VERSION 3.24)

project(stockBacktester
        VERSION 0.1.0
        LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(Cmake/CompilerWarnings.cmake)
include(Cmake/Sanitizers.cmake)
include(Cmake/Versioning.cmake)        # writes Src/App/version.h

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Widgets Charts Test LinguistTools)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt    CONFIG REQUIRED)
find_package(duckdb CONFIG REQUIRED)
find_package(Lua    CONFIG REQUIRED)
# sol2 is header-only; vcpkg port available

qt_standard_project_setup()

add_subdirectory(Src/Backend/Core)
add_subdirectory(Src/Backend/Data)
add_subdirectory(Src/Backend/Indicators)
add_subdirectory(Src/Backend/Strategy)
add_subdirectory(Src/Backend/Engine)
add_subdirectory(Src/Backend/Metrics)
add_subdirectory(Src/Backend/Bindings)
add_subdirectory(Src/Frontend)
add_subdirectory(Src/App)
add_subdirectory(Src/Launcher)
add_subdirectory(Src/Plugins)

if (BTE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()

include(Cmake/Packaging.cmake)         # CPack config per OS
```

### 1.2 `CMakePresets.json` highlights

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "windows-msvc-x64",
      "generator": "Visual Studio 17 2022",
      "architecture": "x64",
      "toolchainFile": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
      "cacheVariables": { "VCPKG_TARGET_TRIPLET": "x64-windows" }
    },
    { "name": "macos-arm64", "generator": "Xcode",
      "cacheVariables": { "CMAKE_OSX_ARCHITECTURES": "arm64",
                          "CMAKE_OSX_DEPLOYMENT_TARGET": "12.0" } },
    { "name": "macos-x64", "generator": "Xcode",
      "cacheVariables": { "CMAKE_OSX_ARCHITECTURES": "x86_64",
                          "CMAKE_OSX_DEPLOYMENT_TARGET": "12.0" } },
    { "name": "linux-clang-x64", "generator": "Ninja",
      "cacheVariables": { "CMAKE_C_COMPILER": "clang",
                          "CMAKE_CXX_COMPILER": "clang++" } },
    { "name": "dev", "inherits": "linux-clang-x64",
      "cacheVariables": { "BTE_SANITIZERS": "address,undefined",
                          "BTE_BUILD_TESTS": "ON" } }
  ]
}
```

### 1.3 vcpkg manifest (`vcpkg.json`)

```json
{
  "name": "stock-backtester",
  "version": "0.1.0",
  "dependencies": [
    { "name": "qt6-base",     "features": ["widgets", "concurrent"] },
    { "name": "qt6-charts" },
    { "name": "qt6-tools" },
    "duckdb",
    "spdlog",
    "fmt",
    "lua",
    "sol2",
    "nlohmann-json"
  ]
}
```

CI overrides `VCPKG_ROOT` per runner. Local devs run `git submodule update --init` to fetch vcpkg pinned to a known commit.

---

## 2. CI pipeline (GitHub Actions)

`.github/workflows/build.yml` runs four matrix jobs on every PR:

| Job | Runner | Steps |
|---|---|---|
| Windows | `windows-2022` | configure → build → ctest → `windeployqt` → upload artifact |
| macOS arm64 | `macos-14` | configure → build → ctest → `macdeployqt` → upload |
| macOS x64 | `macos-13` | same |
| Linux x64 | `ubuntu-22.04` | configure → build → ctest → AppImage → upload |

Each job uploads the built artifact and the SDK zip. A separate **release** workflow (`release.yml`) triggers on a `v*.*.*` git tag:

1. Re-runs all matrix jobs.
2. Generates checksums (`sha256sum`).
3. Codesigns macOS and Windows **if credentials are configured** in secrets (optional; see §7).
4. Creates a **draft** GitHub Release with body from `Docs/Governance/CHANGELOG.md`.
5. Uploads:
   - `stockBacktester-<ver>-windows-x64.zip` and `.msi`
   - `stockBacktester-<ver>-macos-arm64.dmg` and `.tgz`
   - `stockBacktester-<ver>-macos-x64.dmg` and `.tgz`
   - `stockBacktester-<ver>-linux-x64.AppImage` and `.tar.gz`
   - `stockBacktesterLauncher-<ver>-<os>-<arch>.zip` (Launcher binary)
   - `bte-plugin-sdk-<ver>-<os>-<arch>.zip` (for each OS/arch)
   - `release-manifest.json` (machine-readable index — see §4)

---

## 3. Per-OS packaging

We ship **portable archives** + **native installers**, side by side. The Launcher prefers the portable forms (zip / tarball / .app) so it can manage many versions in one folder.

### 3.1 Windows

- **MSI installer** built with [WiX 4](https://wixtoolset.org/) for first-time users. Installs to `%LOCALAPPDATA%\stockBacktester\`. Does **not** require admin.
- **Portable zip** with the result of `windeployqt`: `stockBacktester.exe`, all DLLs (Qt + deps), plugins folder, `qt.conf`. Unzip-and-run.
- Codesigning via SignPath Foundation (free for OSS) if approved; otherwise unsigned (SmartScreen warning).

### 3.2 macOS

- **`.app` bundle** produced by `macdeployqt`.
- **`.dmg`** installer drag-to-Applications.
- **Portable `.tgz`** of the `.app` bundle for the Launcher.
- Distribution: GitHub Releases download + self-hosted Homebrew tap.
- Not codesigned or notarized (no budget). Users bypass Gatekeeper via right-click → Open or `xattr -d com.apple.quarantine`.
- Hardened runtime entitlements file included for future codesigning readiness.

### 3.3 Linux

- **AppImage** (`linuxdeploy --plugin qt`). Single executable file; users `chmod +x` and run. Works on most distros with glibc 2.31+.
- **`.tar.gz`** of the same payload (Launcher uses this).
- GPG detached signature provided (free).
- We don't ship `.deb`/`.rpm` until there's demand — AppImage covers the "works everywhere" promise.

### 3.4 Plugin SDK (cross-cutting)

For each OS/arch we also build the SDK zip described in `08`. Same release, separate asset.

---

## 4. Release manifest

To make the Launcher's job trivial, every Release uploads a **`release-manifest.json`** alongside the binaries:

```json
{
  "version": "0.3.0",
  "released": "2026-08-01T12:00:00Z",
  "channel": "stable",
  "minLauncherVersion": "0.1.0",
  "abi": { "plugin": 1, "luaApi": 1 },
  "assets": [
    {
      "platform": "windows",
      "arch": "x64",
      "kind": "portable",
      "url": "https://github.com/<owner>/<repo>/releases/download/v0.3.0/stockBacktester-0.3.0-windows-x64.zip",
      "size": 84211324,
      "sha256": "abcd..."
    },
    {
      "platform": "macos",
      "arch": "arm64",
      "kind": "portable",
      "url": ".../stockBacktester-0.3.0-macos-arm64.tgz",
      "size": 78901234,
      "sha256": "..."
    },
    {
      "platform": "macos",
      "arch": "x64",
      "kind": "portable",
      "url": ".../stockBacktester-0.3.0-macos-x64.tgz",
      "size": 76543210,
      "sha256": "..."
    },
    {
      "platform": "linux",
      "arch": "x64",
      "kind": "appimage",
      "url": ".../stockBacktester-0.3.0-linux-x64.AppImage",
      "size": 92314444,
      "sha256": "..."
    }
  ],
  "changelog": "https://github.com/<owner>/<repo>/releases/tag/v0.3.0"
}
```

The Launcher fetches `https://api.github.com/repos/<owner>/<repo>/releases` (no auth — we only need public data) and reads the manifest for each release.

---

## 5. The Launcher (`stockBacktesterLauncher`)

A small Qt app that is the user's **single install point**. Downloaded once; manages all versions thereafter. Lives in the same repo as the main app with an independent version number (defined in `Src/Launcher/CMakeLists.txt`).

### 5.1 What it does

1. **Lists installed versions** (each is a folder under the version root — see §5.3).
2. **Lists available versions** from GitHub Releases (cached for 10 min; offline fallback shows cached data + warning).
3. **Installs** a version by downloading its archive, verifying SHA-256, and extracting to `<versionRoot>/<ver>/`.
4. **Switches** the active version by updating `<versionRoot>/active.json`.
5. **Launches** the active version (or any version on demand).
6. **Removes** old versions (with a confirm).
7. **Shows** changelog and ABI info per release so users see what changed.
8. **Auto-downloads** latest stable in background (optional user setting).

### 5.2 UI

A single window, list-on-left + detail-on-right:

```
┌─ Stock Backtester Launcher ─────────────────────────────────────┐
│ Installed                          │  v0.3.0 (active)            │
│  ● v0.3.0    [active]              │  Released: 2026-08-01       │
│    v0.2.4                          │  Plugin ABI: 1              │
│    v0.2.0                          │                             │
│ ─────────────────                  │  Changelog:                 │
│ Available                          │  - replay scrubbing fast    │
│    v0.3.1   (new)   [Install]      │  - new ATR indicator        │
│    v0.4.0-beta      [Install]      │                             │
│                                    │  [ Launch ]  [ Set Active ] │
│                                    │  [ Remove ]  [ Open Folder ]│
└─────────────────────────────────────────────────────────────────┘
```

A "Channels" filter lets the user opt into pre-releases (read from GitHub's `prerelease` flag).

### 5.3 Disk layout

| OS | `<versionRoot>` |
|---|---|
| Windows | `%LOCALAPPDATA%\stockBacktester\versions\` |
| macOS | `~/Library/Application Support/stockBacktester/versions/` |
| Linux | `~/.local/share/stockBacktester/versions/` |

```
versions/
├── 0.2.0/
│   └── (extracted portable bundle)
├── 0.2.4/
├── 0.3.0/
└── active.json     # { "version": "0.3.0", "executable": "..." }
```

User data (`<userData>` from `01`) is **shared across all versions**. Settings, strategies, plugins persist when the user upgrades or downgrades. Schema migrations live in the app and are forward-compatible (we never break older saved strategies on minor upgrades).

### 5.4 "Active" handling

Cross-platform compatible without admin rights:

- **All OSes (default)**: `active.json` plus a small launcher shim. The shim is what users put on their PATH / Dock. It reads `active.json` and `exec`s the right executable.
- **macOS bonus**: optionally maintain a symlink at `~/Applications/stockBacktester.app -> versions/<active>/stockBacktester.app` so Spotlight finds it. Falls back to the shim.
- **Linux bonus**: optionally write a `.desktop` file pointing at the active version.

Default user flow: **double-click the Launcher**, click **Launch**, the app opens. The user never deals with `active.json` directly.

### 5.5 Update logic

Pseudocode:

```cpp
auto manifest = github.fetchReleaseManifest("v0.3.1");
auto asset    = pickAssetForHost(manifest);     // matches OS + arch + kind

auto temp = downloadWithProgress(asset.url, /*toDir*/ tempDir);
if (sha256(temp) != asset.sha256) return error("checksum mismatch");

extract(temp, versionRoot / "0.3.1");
removeFile(temp);

if (userClickedSetActive) writeActive("0.3.1");
```

Resumable downloads via HTTP `Range`; partial files in `tempDir` are reused on retry.

### 5.6 Minimum-version field

`release-manifest.json` carries `minLauncherVersion`. If a release targets a Launcher feature the user doesn't have (e.g. new download protocol), the Launcher prompts the user to update the Launcher first. The Launcher itself updates by writing the new launcher to a side file, then atomically renaming on next start (Windows: pending-rename, macOS/Linux: rename-over-exe).

---

## 6. Versioning policy

- **Backtester app**: semver. Major break means strategies / plugins built before may not work without a port.
- **Launcher**: independent semver (tracked in `Src/Launcher/CMakeLists.txt`). Updated only when Launcher code changes.
- **Plugin ABI**: independent integer. Major bump only on layout/signature change. Multiple app majors can share one plugin major.
- **Lua API** (`bte.apiVersion`): independent integer (`08` §8).
- **Python strategy API** (`pythonApiVersion`): independent integer once the Python host ships (`05`, `08` §8).
- **Data schema**: owned by the Python pipeline; the C++ side reports what it expects and warns on drift (`04`).

These **independent version tracks** are surfaced in **Help → About** (and the Plugins tab) so users know what script and native extensions must target.

---

## 7. Code signing & notarization

- **macOS**: Developer ID Application certificate, hardened runtime, `notarytool` notarization with stapling. Without these, Gatekeeper blocks first-launch.
- **Windows**: EV or OV code-signing certificate ([SignPath](https://signpath.io/) offers free signing for verified OSS projects). Without signing, SmartScreen scares users for weeks.
- **Linux**: AppImages signed with `gpg --detach-sign`; the Launcher verifies the GPG signature when configured to do so.

Signing keys live in GitHub Actions secrets, never in the repo. CI steps are conditional (`if: secrets.APPLE_CERT != ''`) so builds succeed without credentials.

**Current status (ADR 0005):** macOS and Windows signing are deferred — no budget. Linux GPG signing will be implemented (free). If the project gains sponsorship or SignPath approval, signing can be enabled without architectural changes.

---

## 8. Local "from-source" path

For developers, the Launcher is unnecessary:

```bash
git clone https://github.com/<owner>/Stock-Back-Test-System.git
cd Stock-Back-Test-System
git submodule update --init
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./Output/dev/Src/App/stockBacktester
```

A single `Cmake/Versioning.cmake` generates `version.h` from `git describe --tags --dirty` so dev builds clearly say `0.3.0-12-gabcdef-dirty` in About.

---

## 9. Telemetry

**None by default.** No phone-home. The Launcher contacts only `api.github.com` and the asset URLs the user explicitly chose to download. We may add opt-in crash reporting later (Sentry); not Phase 1.

---

## 10. Tests / CI gates

- Build matrix green is a hard gate for merging to main.
- Smoke test per OS: launch app, open each tab (**Strategies, Backtest, Replay, Screener, Plugins, Logs** per `02`), close cleanly. Performed by Qt Test in headless mode (`QT_QPA_PLATFORM=offscreen` on Linux, `-platform offscreen` everywhere).
- Launcher integration test (Linux only — sufficient): with a fake GitHub server, install two versions, set active, launch each, remove one. Asserts `active.json` correctness.
- Reproducibility: backtest a fixture run on each OS; compare `metrics.json` byte-for-byte (we already promised determinism in `07`). Drift across OS = release blocker.

---

## 11. Task Tracker

### Phase 1 — Research

| # | Task | Priority | Status |
|---|------|----------|--------|
| 1 | [Research] CMake 3.24+ / vcpkg manifest mode / Qt6 CMake integration | **High** | ⬜ |
| 2 | [Research] DuckDB vcpkg port availability & CMake target name | **High** | ⬜ |
| 3 | [Research] Cross-platform packaging (WiX 4, macdeployqt, linuxdeploy) | Medium | ⬜ |
| 4 | [Research] GitHub Releases API rate limits & offline fallback strategy | Medium | ⬜ |
| 5 | [Research] Cross-platform determinism: floating-point & JSON serialization | Low | ⬜ |

### Phase 2 — Build System + POC

| # | Task | Priority | Status |
|---|------|----------|--------|
| 6 | [Build] Top-level CMakeLists.txt — integrate vcpkg + existing FetchContent | **High** | ⬜ |
| 7 | [Build] CMakePresets.json — extend with multi-platform presets | **High** | ⬜ |
| 8 | [Build] vcpkg.json manifest — pin all third-party dependencies | **High** | ⬜ |
| 9 | [Build] Cmake/Versioning.cmake — generate version.h from git describe | Medium | ⬜ |
| 10 | [Build] Cmake/CompilerWarnings.cmake — enhance (already exists) | Low | ⬜ |
| 11 | [Build] Cmake/Sanitizers.cmake — enhance (already exists) | Low | ⬜ |
| 12 | [Build] Handle non-existent modules — optional add_subdirectory mechanism | **High** | ⬜ |
| 13 | [POC] Minimal working build: Qt6 + DuckDB + spdlog via vcpkg | **High** | ⬜ |

### Phase 3 — CI/CD, Packaging, Signing, Testing

| # | Task | Priority | Status |
|---|------|----------|--------|
| 14 | [Build] Cmake/Packaging.cmake — CPack configuration | Medium | ⬜ |
| 15 | [CI/CD] .github/workflows/build.yml — matrix build | **High** | ⬜ |
| 16 | [CI/CD] .github/workflows/release.yml — align with RELEASE_PROCESS.md | **High** | ⬜ |
| 17 | [Packaging] Windows MSI + portable zip | Medium | ⬜ |
| 18 | [Packaging] macOS .app + .dmg + .tgz | Medium | ⬜ |
| 19 | [Packaging] Linux AppImage + .tar.gz | Medium | ⬜ |
| 20 | [Manifest] release-manifest.json schema definition & generation script | **High** | ⬜ |
| 21 | [Signing] macOS Developer ID + notarization | Low* | ⬜ |
| 22 | [Signing] Windows code signing (SignPath) | Low* | ⬜ |
| 23 | [Signing] Linux GPG signing | Low | ⬜ |
| 24 | [Testing] Smoke test — headless launch per OS | **High** | ⬜ |
| 25 | [POC] Basic packaging: macdeployqt → .tgz executable verification | Medium | ⬜ |

### Phase 4 — Launcher

| # | Task | Priority | Status |
|---|------|----------|--------|
| 26 | [Launcher] UI — version list + detail panel + action buttons | **High** | ⬜ |
| 27 | [Launcher] Fetch available versions (with offline/rate-limit fallback) | **High** | ⬜ |
| 28 | [Launcher] Download + SHA-256 verify + extract | **High** | ⬜ |
| 29 | [Launcher] active.json management + shim executable | **High** | ⬜ |
| 30 | [POC] Minimal Launcher — end-to-end flow verification | **High** | ⬜ |
| 31 | [Testing] Launcher integration test — fake server | Medium | ⬜ |
| 32 | [Testing] Cross-OS determinism test — metrics.json comparison | Medium | ⬜ |

### Phase 5 — Polish & Optional

| # | Task | Priority | Status |
|---|------|----------|--------|
| 33 | [Launcher] minLauncherVersion check + self-update mechanism | Medium | ⬜ |
| 34 | [Launcher] Channels filter (stable / pre-release) | Low | ⬜ |
| 35 | [Launcher] macOS ~/Applications symlink (optional) | Low | ⬜ |
| 36 | [Launcher] Linux .desktop file generation (optional) | Low | ⬜ |
| 37 | [Packaging] Plugin SDK zip per OS/arch | Low | ⬜ |

*\* #21, #22 downgraded due to OSS no-spend decision. Only pursue if a free path exists.*

---

## 12. Timeline (3–5 hrs/week)

Estimated schedule at ~4 hours/week. Issues reference `09_Issue_Plan.md`.

| Week | Hours | Issue | Deliverable |
|------|-------|-------|-------------|
| 1 | 4 | A | Research: vcpkg + Qt6 + DuckDB findings documented |
| 2 | 4 | B, D | Packaging/API research; Versioning.cmake written |
| 3 | 5 | C | vcpkg.json + CMakeLists.txt + CMakePresets.json integrated |
| 4 | 4 | E | POC builds and runs (Qt6 window + DuckDB + spdlog) |
| 5 | 5 | F, K | CI matrix build green; active.json + shim working |
| 6 | 5 | G | CPack packaging: MSI/zip, .app/.tgz, AppImage |
| 7 | 4 | H | release.yml + release-manifest.json schema |
| 8 | 4 | I, J | POC packaging verified; smoke test in CI |
| 9 | 5 | L | Launcher fetch + download + SHA-256 verify |
| 10 | 5 | M | Launcher UI + end-to-end POC flow |
| 11 | 3 | N | Launcher integration test (fake server) |

**Total: ~48 hours across 11 weeks.**

Buffer: +2–3 weeks for unexpected blockers (vcpkg port issues, CI quirks).
Realistic delivery: **~14 weeks (3.5 months)** from start.

### Milestones

| Milestone | Target | Gate |
|-----------|--------|------|
| Build works | Week 4 | POC app compiles and runs via vcpkg |
| CI green | Week 5 | All 4 matrix jobs pass |
| Packages ship | Week 8 | Portable archives work on clean machines |
| Launcher MVP | Week 11 | End-to-end: install → set active → launch |

---

## 13. Decisions Made

Full rationale in [ADR 0005](../Decisions/0005-build-distribution-launcher-decisions.md).

### ✅ #1 — Qt LGPL dynamic linking (no commercial license)

- Use LGPL dynamic linking for all dependencies
- Windows vcpkg triplet = `x64-windows` (all dynamic)
- windeployqt deploys Qt DLLs + all other DLLs alongside exe
- Simplest setup, no custom triplet needed

### ✅ #2 — macOS arm64/x64 separate builds (no Universal Binary)

- CI uses `macos-14` (arm64) and `macos-13` (x64) in parallel
- release-manifest.json has two macOS assets
- Launcher detects `uname -m` to pick the correct download
- Distribution: self-hosted Homebrew tap + .dmg on GitHub Releases
- No codesign — users run `xattr -d com.apple.quarantine` or right-click → Open on first launch

### ✅ #3 — Launcher same repo, OTA-style

- Same repo, shared release; Launcher has independent version number (defined in `Src/Launcher/CMakeLists.txt`)
- App updates: Launcher auto-detects → background download → user one-click switch
- Launcher self-update: `minLauncherVersion` triggers → download new Launcher → side file → restart atomic rename
- Optional "auto-download latest stable" feature

### ✅ #4 — Code signing: no spend (OSS project)

- macOS: No codesign/notarize. Users right-click → Open to bypass Gatekeeper.
- Windows: No certificate purchase. May try SignPath free OSS plan, not required. SmartScreen warning dismissed manually.
- Linux: GPG signing is free, will do.
- #21, #22 downgraded to nice-to-have if free.
