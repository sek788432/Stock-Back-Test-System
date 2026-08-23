# Build and Distribution

This specification owns supported release platforms, artifacts, packaging,
runtime profiles, and public-release blockers. Developer build and CI commands
belong to [`../BUILD.md`](../BUILD.md) and [`CiDevFlow.md`](CiDevFlow.md).

## Delivery status

- **Implemented:** local CMake builds and CI verification on repository-defined
  platforms.
- **Accepted design / not implemented:** the signed full-install packages and
  immutable runtime/data manifests below.
- **Blocked:** no public data-bearing release may ship without documented
  redistribution rights and a verified redistribution-cleared split manifest.
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

The installer contains the application, required dynamically linked Qt
libraries, licenses, one immutable default Python runtime profile, and a
verified release-data manifest when redistribution is permitted. It does not
contain provider credentials, system Python integration, arbitrary packages,
or mutable user data.

Writable content uses Qt's `QStandardPaths::AppLocalDataLocation`:

```text
Strategies/Active
Strategies/Trash
Results/Active
Results/Trash
DataSegments
RuntimeProfiles
```

The installation directory is read-only at runtime. Upgrade and uninstall must
not silently remove user strategies, results, referenced Data Segments, or
referenced Runtime Profiles.

## Immutable manifests

Every release records exact application, toolchain, Qt, Python runtime, package,
data-segment, calendar, and split-manifest versions and hashes, plus licenses
and an SBOM. A Runtime Profile contains CPython, project-owned `stockbt`, NumPy,
and pandas only. A release never resolves dependencies from the network.

Strategies pin the Runtime Profile needed for future execution. Results embed
exact strategy source but do not pin the runtime because replay never executes
it. An unreferenced Runtime Profile first moves to recoverable Trash and is
eligible for permanent removal after 30 days.

## Release verification

Before publication:

- build, install, launch, and uninstall the exact artifact on every supported
  baseline;
- verify signatures, hashes, licenses, SBOM, runtime manifest, and absence of
  secrets;
- verify user data survives upgrade and uninstall as documented;
- run one Selectable Conditions and one Python result workflow when those
  features are implemented;
- reopen a stored result without starting the engine or Python;
- verify packaging is offline and no updater or launcher is installed;
- verify every included Data Segment is covered by redistribution evidence and
  the exact split manifest.

Until all applicable checks exist and pass, packaging remains **Planned** and a
public data-bearing release remains **Blocked**.
