# 08 — Native Plugin System

> **Status:** Planned. No release-grade native plugin loader, SDK, ABI gate, trust prompt, or compatibility suite is implemented. Plugins are not required for V1.

## Purpose

Native plugins are a future extension seam for capabilities that cannot reasonably live in the application. They are not the strategy-script mechanism:

- Selectable Conditions compile to the project-owned C++ engine (`05`).
- Python strategies run through the managed Python worker (`05`).
- V1 market data comes only from the frozen managed snapshot (`04`).

## Accepted boundary

- Native plugins execute with the user's full local privileges; they are not sandboxed.
- Loading requires an explicit user action and a trust prompt showing the file path and SHA-256 hash.
- The host owns lifetime, logging, cancellation, and error reporting.
- The public ABI is a narrow C ABI. A C++ SDK may wrap it without becoming the ABI.
- ABI compatibility uses independent major/minor numbers. Major mismatch rejects loading; a newer minor version may only use optional negotiated capabilities.
- Exceptions, Qt objects, STL containers, allocators, and ownership of raw pointers must not cross the ABI.
- Every resource crosses the boundary through an explicit create/destroy pair or an opaque host-owned handle.

## V1 exclusions

- Custom market-data providers.
- Custom fill, accounting, margin, or portfolio engines.
- Strategy execution through native plugins.
- Downloading or updating plugins from the application.
- Hot reload while a backtest is running.

These exclusions protect deterministic replay and the managed-data contract. Adding one requires an ADR and coordinated changes to the data, engine, persistence, release, and threat-model specs.

## Future load flow

1. Resolve and canonicalize the selected file.
2. Compute and display its hash.
3. Obtain explicit trust for that exact hash.
4. Load the library and query its ABI descriptor.
5. Reject incompatible or malformed descriptors before invoking plugin behavior.
6. Register only negotiated capabilities.
7. Unload only after all plugin-owned handles have been released.

Trust is invalidated whenever the file hash changes.

## Required verification before implementation is merge-blocking

- Positive: compatible plugin loads, registers a capability, and unloads cleanly.
- Negative: wrong ABI, missing symbol, malformed descriptor, throw across callback, and changed untrusted hash are rejected without corrupting host state.
- Boundary: zero capabilities, maximum declared descriptor sizes, repeated load/unload, cancellation, and shutdown ordering.
- Sanitizer runs cover ownership and callback lifetime.
- A symbol/export audit proves that only the documented ABI is public.

## Not implemented yet

- ABI headers and SDK.
- Loader and trust database.
- Compatibility fixtures.
- Export/symbol CI gate.
- Packaging, signing, discovery, and update policy.

Until those items exist, documentation and UI must not describe native plugins as available.
