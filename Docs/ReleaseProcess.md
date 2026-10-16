# Release Process

> **Status:** Target process. Release packaging, signing, managed-runtime
> assembly, and manual publication are not implemented yet.

## Versioning

- Application releases use semantic versioning.
- Strategy API, result schema, and data manifest are versioned independently.
- A breaking stored-artifact change requires a migration or an explicit compatibility rejection.

## Release readiness

- Canonical specs and changelog match implemented behavior.
- All currently configured required checks pass on `main`.
- The public-behavior test inventory has positive, negative, and meaningful boundary coverage for changed behavior.
- Every bug fix or behavior change has a regression test.
- Dependency licenses and notices are complete.
- Managed-data redistribution rights and split provenance are approved.
- The data snapshot has a validated, content-addressed manifest.
- The bundled Python runtime and packages pass compatibility tests.
- Install, launch, sample backtest, saved result, replay, and uninstall pass on each supported platform.

## Cut and publish

1. Open a release PR that bumps the version and finalizes the changelog.
2. Merge only after all implemented required checks pass.
3. Create a signed version tag from the merge commit.
4. Build platform artifacts from that tag in a clean environment.
5. Sign/notarize artifacts and the data manifest.
6. Test artifacts on clean supported systems.
7. Publish checksums, notices, compatibility versions, and known limitations with the release.

There is no emergency bypass. A bad release is withdrawn or followed by a tested patch release; user results and data snapshots remain recoverable.

## Not implemented yet

- Windows CI and complete supported-platform release matrix.
- Installer generation and clean-machine smoke automation.
- Code signing, notarization, and signed manifest flow.
- Managed Python-runtime assembly and package lock validation.
- Managed-data licensing and release validation gates.
- Manual artifact publication and withdrawal procedure.

V1 has no updater, launcher, delta patcher, or automated publication contract.

See [`Specs/BuildDistribution.md`](Specs/BuildDistribution.md) for the release
contract and [`Specs/CiDevFlow.md`](Specs/CiDevFlow.md) for current gate truth.
