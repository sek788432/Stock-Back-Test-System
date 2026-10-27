# Build and Distribution

This specification owns supported release platforms, artifacts, packaging,
runtime profiles, and public-release blockers. Developer build and CI commands
belong to [`../BUILD.md`](../BUILD.md) and [`CiDevFlow.md`](CiDevFlow.md).

## Delivery status

- **Implemented:** local CMake builds and CI verification on repository-defined
  platforms.
- **Implemented locally:** the locked CPython profile builder, runtime
  integrity manifest, and macOS Apple Silicon `.app` bundle integration below.
  The signed full-install packages and immutable release-data manifest are
  still planned. Windows and Linux builds keep Python Strategy unavailable
  until their worker/runtime profiles receive separate acceptance.
- **Blocked:** no public product release containing managed data may ship
  without documented redistribution rights, a verified redistribution-cleared
  split manifest, and exact snapshot hashes.
  Qt Charts also cannot ship in the Apache-2.0 application distribution.

## Supported release artifacts

V1 distributes manually downloaded, signed, complete installers only:

| Platform | Supported baseline | Artifact |
|---|---|---|
| Windows | Windows 11 x64 | MSI |
| macOS | macOS 14 or later, Apple Silicon | DMG |
| Linux | Ubuntu 22.04 and 24.04 x64 | AppImage |

An updater, launcher, repair service, delta patcher, package-manager feed,
Windows Arm64 build, Intel macOS build, and other Linux distributions are not
accepted V1 contracts. A future change requires explicit maintainer approval
and coordinated specification and decision updates.

## Package contents and locations

The macOS installer contains the application, required dynamically linked Qt
libraries, licenses, one immutable default Python runtime profile, and a
verified release-data manifest when redistribution is permitted. The first
Python runtime profile targets `aarch64-apple-darwin`; other platform
installers do not enable Python Strategy until their own immutable profile and
worker acceptance exist. No installer contains provider credentials, system
Python integration, arbitrary packages, or mutable user data.

Writable content uses Qt's `QStandardPaths::AppLocalDataLocation`:

```text
Strategies/PythonDraft
Results/Active
Results/Trash
Results/Verifications
DataSegments
```

The installation directory is read-only at runtime. Upgrade and uninstall must
not silently remove user strategies, results, referenced Data Segments, or
referenced Runtime Profiles.

The first milestone stores only one recoverable Python editor draft; a saved
Strategy library and its `Strategies/Active` / `Strategies/Trash` directories
are deferred. The current immutable Python runtime profile remains inside the
read-only application bundle. The `Results/Verifications` directory contains
the bounded `.bteverify` coordinator records described in
[`EngineReplayPnL.md`](EngineReplayPnL.md#83-reproducibility-evidence); these
records are metadata and are never listed or published as Results.

## Immutable manifests

Every release records exact application, toolchain, Qt, Python runtime, package,
data-segment, calendar, and split-manifest versions and hashes, plus licenses
and an SBOM. The direct user-visible runtime roots are CPython,
project-owned `stockbt`, NumPy, and pandas. Their explicitly pinned transitive
packages and profile-stable timezone data are included; no arbitrary package
is included. A release never resolves dependencies from the network.

The first milestone installs the profile read-only inside the signed
application bundle at `Contents/Resources/RuntimeProfiles/<runtimeProfileId>`.
It contains CPython 3.14.9, project-owned `stockbt`, NumPy 2.5.4, pandas 3.0.6,
and the locked transitive/timezone packages python-dateutil 2.9.0.post0, six
1.17.0, and tzdata 2026.5. The exact upstream files are pinned below. The
profile is assembled offline from verified local artifacts, and the release
contains its canonical manifest, installed-file hashes, retained notices, and
SPDX SBOM. The builder and app verifier require the exact reviewed SPDX license
set for every pinned component, reject unapproved components and hash changes,
and never build a package from source or resolve runtime dependencies over the
network. Retained notices and the SBOM provide attribution; redistribution
clearance remains a separate release gate. `pip`, `ensurepip`, build caches,
and package installers are removed from the shipped profile after assembly.

| Component | Exact selected artifact | SHA-256 | Declared license |
|---|---|---|---|
| CPython 3.14.9, python-build-standalone release `20261021`, macOS Apple Silicon, standard `install_only` | [Release archive](https://github.com/astral-sh/python-build-standalone/releases/download/20261021/cpython-3.14.9%2B20261021-aarch64-apple-darwin-install_only.tar.gz) | `226d696642a8a53d03b4d5b255b1c319b1575a6d44bd723a953c9f507755e36a` | PSF-2.0 plus each bundled-library notice in the archive |
| NumPy 2.5.4, CPython 3.14, macOS 11+ ARM64 wheel | [PyPI release](https://pypi.org/project/numpy/2.5.4/) | `ffa6ce09a1c6a08e9667dd9c97aa0b14184e8d18f2a14b78b2a2328c9147f076` | BSD-3-Clause and bundled component notices |
| pandas 3.0.6, CPython 3.14, macOS 11+ ARM64 wheel | [PyPI release](https://pypi.org/project/pandas/3.0.6/) | `ff51a4459ed036e93d1eb1bb5e6e7b28685d3cb6b7c12b91c05b31024e234729` | BSD-3-Clause |
| python-dateutil 2.9.0.post0, universal wheel | [PyPI release](https://pypi.org/project/python-dateutil/2.9.0.post0/) | `a8b2bc7bffae282281c8140a97d3aa9c14da0b136dfe83f850eea9a5f7470427` | Apache-2.0 OR BSD-3-Clause |
| six 1.17.0, universal wheel | [PyPI wheel](https://pypi.org/project/six/1.17.0/) | `4721f391ed90541fddacab5acf947aa0d3dc7d27b2e1e8eda2be8970586c3274` | MIT |
| tzdata 2026.5, universal wheel | [PyPI wheel](https://pypi.org/project/tzdata/2026.5/) | `b683bd1b6659ddcd810ff02ad09ba821d4bf1065072805063eb35c49617905ac` | Apache-2.0 |
| project-owned `stockbt` | Built from the locked application revision; its package/source digest is in the runtime manifest | Per-build manifest | Apache-2.0 |

PyPI version pages identify the selected platform wheels and their published
SHA-256 values. PyPI file hashes are the lock authority. Runtime
Profile identity is SHA-256 over a canonical manifest containing platform,
Python ABI, exact package names/versions, artifact and installed-file hashes,
license identifiers/notices, `stockbt` source digest, seed policy, and profile
builder version. The profile hash is assigned only after the pinned files are
downloaded, verified, installed, import-tested on macOS 14 Apple Silicon, and
rehashed as an installed tree.

The first milestone has one local editor draft and `.py` load/export only, so
there is no Strategy library profile pin or Runtime Profile trash lifecycle.
Results retain the exact profile ID/hash but do not pin installed runtime
files: Replay never executes Python. A future saved/versioned Strategy
library must define profile retention before it is implemented.

## Release verification

Before publication:

- build, install, launch, and uninstall the exact artifact on every supported
  baseline;
- verify signatures, hashes, licenses, SBOM, runtime manifest, and absence of
  secrets;
- on macOS 14+ Apple Silicon, verify the CPython archive and every wheel hash,
  assemble from a pre-populated cache with networking disabled, import the
  pinned packages, verify the managed timezone database, and run success,
  timeout, cancel, missing-profile, and tampered-profile Worker tests; on
  Windows/Linux, verify that Python Strategy is unavailable without falling
  back to a system interpreter;
- verify user data survives upgrade and uninstall as documented;
- run the implemented Selectable Conditions and Python result workflows;
- reopen a stored result without starting the engine or Python;
- verify packaging is offline and no updater or launcher is installed;
- verify every included Data Segment is covered by redistribution evidence and
  the exact split manifest.

Until all applicable checks exist and pass, packaging remains **Planned** and a
public data-bearing product release remains **Blocked**.
