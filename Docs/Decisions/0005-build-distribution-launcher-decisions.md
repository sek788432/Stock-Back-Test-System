# 0005 — Build, distribution, and launcher deployment decisions

- **Status**: Superseded by 0011
- **Date**: 2026-05-29
- **Deciders**: founding team
- **Supersedes**: —
- **Superseded by**: 0011

## Context

Spec 09 defines the build system, packaging, and launcher architecture. Several design choices needed resolution before implementation could begin. The project is OSS with zero budget for paid tooling or certificates.

## Decision

Four decisions were made:

### 1. Qt LGPL dynamic linking (no commercial license)

Use Qt under LGPL-3 with **dynamic linking** on all platforms. All vcpkg dependencies are also dynamically linked for simplicity.

- Windows vcpkg triplet: `x64-windows` (all dynamic).
- `windeployqt` deploys all required DLLs alongside the executable.
- Users can replace Qt shared libraries (LGPL compliance).
- No custom triplet needed — simplest possible setup.

### 2. macOS arm64 and x64 as separate builds (no Universal Binary)

Build two independent binaries rather than a single fat Universal Binary.

- CI matrix: `macos-14` (arm64 runner) and `macos-13` (x64 runner) in parallel.
- `release-manifest.json` lists two macOS assets (one per arch).
- Launcher detects host architecture (`uname -m`) and normalizes to canonical manifest names (`x86_64` → `x64`, `aarch64` → `arm64`) to select the correct download.

### 3. Launcher same repo, OTA-style updates

The Launcher lives in the same repository as the main app but maintains an independent version number (defined in `Src/Launcher/CMakeLists.txt`).

- **App updates**: Launcher polls GitHub Releases → background download → user one-click switch.
- **Launcher self-update**: `minLauncherVersion` field in `release-manifest.json` triggers update → download to side file → atomic rename on next restart.
- Launcher binary is uploaded as a release asset alongside the app.
- Optional "auto-download latest stable" feature for hands-free OTA.

### 4. Code signing: no spend

As an OSS project with no budget:

- **macOS**: No codesign or notarization. Users bypass Gatekeeper via right-click → Open.
- **Windows**: No EV/OV certificate purchase. May attempt SignPath free OSS plan if verified, but not a blocker. SmartScreen warning dismissed manually by users.
- **Linux**: GPG detached signatures (free) will be provided.

## Consequences

**Positive:**

- Zero cost. No certificates, no Apple Developer Program fee, no commercial Qt license.
- Dynamic linking satisfies LGPL without legal risk.
- Separate macOS builds keep CI fast (parallel) and binaries small.
- OTA-style launcher gives users a seamless update experience without manual downloads.
- Same-repo simplifies release coordination (one tag, one workflow).

**Negative:**

- macOS users see Gatekeeper warning on first launch (friction for non-technical users).
- Windows users see SmartScreen warning until reputation builds (or SignPath is set up).
- Separate macOS builds mean Launcher must detect architecture (minor complexity).
- Launcher independent versioning adds a second version track to maintain.

**Mitigations:**

- README and download page will include clear instructions for bypassing Gatekeeper/SmartScreen.
- If the project gains sponsorship, codesigning can be added without architectural changes (CI steps map secret presence to `env` and gate with `if: env.APPLE_CERT != ''`-style conditions).
- Architecture detection is a single `uname -m` call; trivial to implement.

## Alternatives considered

| Decision | Alternative | Why rejected |
|----------|------------|--------------|
| Qt licensing | Commercial license ($300+/mo) | OSS, no budget |
| Qt licensing | Static linking under LGPL | Requires relinkable object files and a more complex compliance process than dynamic linking |
| Qt licensing | Custom triplet (Qt dynamic, others static) | Extra complexity for minimal benefit |
| macOS builds | Universal Binary | Doubles CI time, larger binary, no clear user benefit |
| Launcher repo | Separate repository | Adds release coordination overhead for no gain |
| Launcher repo | No launcher (manual downloads) | Poor UX; no OTA capability |
| Code signing | Apple Developer Program ($99/yr) | No budget |
| Code signing | Windows EV cert (~$400/yr) | No budget |

## References

- [`../Specs/09BuildDistributionLauncher.md`](../Specs/09BuildDistributionLauncher.md) §1–§7
- [`Dependencies.md`](Dependencies.md) — Qt 6 listed as LGPL-3
