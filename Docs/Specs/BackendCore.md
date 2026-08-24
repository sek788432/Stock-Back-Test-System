# Backend Core

Core defines the small, dependency-light vocabulary shared by backend modules. This file owns time, financial scalar types, base market/order values, errors, and run status.

## 1. Status

- **Implemented:** UTC millisecond timestamps, `DateRange`, floating-point `Bar`, OHLC helpers, and `Result<T>` exist.
- **Planned migration:** fixed-point domain types, valid-state `Result<T>` including `Result<void>`, order/fill/portfolio values, and canonical serialization.
- **Not release-ready:** current `double` accounting is baseline code, not the accepted financial contract; durable result persistence remains planned.

The implemented timestamp parser still mishandles offsets/fractions/trailing
input, and the current `Result<T>` permits invalid states described in the
repository audit. The contracts below are target behavior until those gaps and
their regression tests land.

## 2. C++ conventions

- Namespaces, functions, methods, and variables use `lowerCamelCase`; types use `UpperCamelCase`; scoped enum values use `lowerCamelCase`.
- New C++ file stems are `UpperCamelCase`; unit tests are `UnitTest_<Thing>.cpp`.
- Project headers use `#pragma once`.
- Paths use `std::filesystem::path`; time uses `std::chrono`.
- Public fallible functions are `[[nodiscard]]` and do not throw across module seams.

## 3. Time

```cpp
using Timestamp = std::chrono::sys_time<std::chrono::milliseconds>;

struct DateRange {
    Timestamp start;
    Timestamp end; // half-open [start, end)
};
```

Core timestamps are UTC. Exchange-session identity uses the versioned US-equities calendar owned by the Release Snapshot. UI localization occurs only at the presentation edge. Parsing must reject invalid calendar dates, unsupported offsets, and trailing input rather than silently truncating them.

## 4. Authoritative numeric types

| Type | Representation | Scale |
|---|---|---|
| `Price` | signed checked `int64_t` | 1 unit = USD 0.000000001 |
| `Quantity` | signed checked `int64_t` | 1 unit = 0.000001 share |
| `Money` | signed checked `int64_t` | 1 unit = USD 0.000001 |
| `Rate` | signed checked `int64_t` | 1 unit = 0.000000001 |

- Overflow is an error; no operation silently wraps.
- General `Money` conversion rounds half-even once at the domain edge.
- Adverse execution costs and margin requirements round conservatively.
- Buy slippage rounds price upward; sell slippage rounds downward.
- Python-facing floats are input ergonomics only. Prices normalize to at most 9 decimal places and quantities to at most 6 before command validation.
- Canonical result values are stored as integer units, never JSON floating-point values.

Floating point remains appropriate for indicators, research statistics, and display. It is not authoritative accounting.

## 5. Market and trading values

The planned canonical values use the strong types above:

```cpp
struct Bar {
    Timestamp ts;
    Price open;
    Price high;
    Price low;
    Price close;
    Quantity volume;
};

enum class OrderSide { buy, sell };
enum class OrderType { market, limit, stop };
enum class TimeInForce { day, gtc };
enum class OrderState { pending, active, filled, cancelled, expired, rejected };
```

V1 does not expose stop-limit, IOC, FOK, partial-fill, or liquidity-model values. OCO and bracket relationships are explicit order-group identifiers, not hidden behavior.

One `Position` exists per symbol and holds signed net `Quantity` plus average `Price`. `Fill` is immutable. `PortfolioSnapshot` is an immutable value containing cash, restricted proceeds, positions, realized/unrealized P&L, and margin state.

The current floating-point `Bar` remains implemented until the migration lands. Its exact object size is not a persistence or IPC contract.

## 6. `Result<T>` and errors

The canonical spelling is:

```cpp
namespace bte::core {
template <typename T>
class Result; // fixed Error payload
}
```

Use `bte::core::Result<T>`, not `Result<T, Error>`. A successful result contains a `T`; a failed result contains a non-`ok` `Error`. Invalid mixed or empty states are not constructible. `Result<void>` represents success without a value.

`Error` contains a stable `ErrorCode`, user-readable message, source location, and optional cause chain. Required codes include invalid input, unavailable data/snapshot/runtime, cancellation, timeout, schema mismatch, strategy validation/runtime/protocol failures, buying-power failure, and result corruption.

Rules:

- `value()` is available only for success; callers must not obtain a value from failure.
- An `ErrorCode::ok` object cannot construct a failed result.
- Public adapters translate filesystem, SQLite, Python, and parsing exceptions into `Error`.
- UI code must preserve failure distinct from an empty successful collection.

## 7. Run status

| Status | Meaning |
|---|---|
| `Completed` | The run ended normally and final metrics are valid. |
| `Failed` | A strategy, protocol, engine, or persistence error stopped execution. |
| `Canceled` | The user canceled execution; committed diagnostics remain non-final. |
| `Interrupted` | Process or write interruption prevented normal finalization. |
| `Incomplete` | Execution ended but a required final condition was unavailable, such as `StaleFinalMark` or `UnliquidatableMarginDeficit`. |

Only `Completed` exposes final performance metrics. Other statuses may expose clearly labeled diagnostic values.

## 8. Canonical serialization

Every persisted/wire value has an explicit field order, integer scale, enum encoding, and schema version. `canonicalResultHash` covers functional records and their ordered identities. It excludes local paths, wall-clock metadata, and SQLite physical layout.

## 9. Verification requirements

Each public behavior requires positive, negative, and boundary unit tests. Any bug fix or intentional behavior change requires a regression test that fails under the prior behavior. Planned checks must not be called enforced until CI actually requires them.
