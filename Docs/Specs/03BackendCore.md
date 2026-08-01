# 03 — Backend Core

The shared vocabulary every other backend module speaks: bars, orders, trades, portfolios, time, error type, logging. Tiny module, but everything else depends on it.

---

## 1. Coding conventions

- Variables, methods, free functions, namespaces: **lowerCamelCase** (`barIndex`, `submitOrder`, `bte::engine`).
- Types (classes, structs, enums, type aliases): **UpperCamelCase** (`Bar`, `OrderType`, `Portfolio`).
- Enum values: **lowerCamelCase** when scoped (`OrderSide::buy`).
- Member fields: **trailing underscore** for private, plain for public structs (`cash_`, vs. `Bar::open`).
- **C++ file names (new code):** **UpperCamelCase** stem, matching the primary type or module unit (`Bar.h`, `Portfolio.cpp`). Extensions: headers `.h`, sources `.cpp`.
- **C++ unit test file names:** `UnitTest_<Thing>.cpp` where `<Thing>` is UpperCamelCase (e.g. `UnitTest_Bar.cpp` for `Bar`). See Docs/Specs/10 for audit pairing with headers.
- **Directory names (repo layout, module trees, generated build tree):** **UpperCamelCase** — e.g. `Src/`, `Docs/`, `Docs/Governance/`, `Tests/`, and the CMake binary directory **`Output/`** (see `CMakePresets.json`; e.g. `Output/dev`).
- **Markdown (`*.md`):** no project-wide naming convention.
- Header guards: `#pragma once` (every supported compiler handles it).
- Namespace: everything in `bte::core` (alias `bte` for short uses).

This matches the user's stated rule and keeps the existing Python pipeline's `lowerCamelCase` style consistent.

---

## 2. Time & timestamps

DuckDB stores `ts` as `TIMESTAMPTZ` (UTC). We mirror that: **all timestamps in the C++ layer are UTC**, represented by:

```cpp
namespace bte::core {

using Timestamp = std::chrono::sys_time<std::chrono::milliseconds>;

struct DateRange {
    Timestamp start;
    Timestamp end;     // half-open: [start, end)
};

}  // namespace bte::core
```

UI formats with the user's locale at the very edge (Qt `QLocale`); core code never localizes.

Helpers in `bte::core::time`:

```cpp
Timestamp parseIso8601(std::string_view s);  // returns Result<Timestamp, Error>
std::string toIso8601(Timestamp ts);
Timestamp fromUnixMillis(int64_t ms);
int64_t   toUnixMillis(Timestamp ts);
```

---

## 3. The bar

```cpp
namespace bte::core {

struct Bar {
    Timestamp ts;        // bar close time, UTC
    double open  = 0.0;
    double high  = 0.0;
    double low   = 0.0;
    double close = 0.0;
    double volume = 0.0;

    constexpr bool isValid() const noexcept {
        return open > 0 && high >= std::max({open, close, low})
            && low  > 0 && low  <= std::min({open, close, high})
            && volume >= 0.0
            && high >= low;
    }
};

struct SymbolBar {
    std::string symbol;
    Bar bar;
};

std::optional<double> typicalPrice(const Bar& bar) noexcept;
std::optional<double> medianPrice(const Bar& bar) noexcept;
std::optional<double> trueRange(const Bar& bar, std::optional<double> prevClose) noexcept;
std::optional<double> trueRange(const Bar& bar) noexcept;

}  // namespace bte::core
```

- `Bar` is trivially copyable. Its object representation contains **no padding beyond `sizeof(Timestamp) + 5 * sizeof(double)`** (`static_assert` in `Bar.h`). With an 8-byte `Timestamp` on LP64, **`sizeof(Bar)` is 48 bytes** (the older “56 bytes” figure assumed a different `Timestamp` width).
- `isValid()` mirrors the Python pipeline's validation (see `FetchDatabento.py`) — same invariant, same source of truth.
- `SymbolBar` is for multi-symbol streams; single-symbol streams stick to `Bar`.
- `trueRange(bar)` equals intrabar `high - low`. **`trueRange(bar, prevClose)`** implements Wilder true range: `max(high - low, |high - prevClose|, |low - prevClose|)` when `prevClose` is set; omit it on the first bar or when no prior close exists (`nullopt`).

---

## 4. Orders, fills, trades

### 4.1 Order

```cpp
enum class OrderSide  { buy, sell };
enum class OrderType  { market, limit, stop, stopLimit };
enum class TimeInForce { day, gtc, ioc, fok };

struct Order {
    uint64_t id = 0;            // assigned by Engine
    std::string symbol;
    OrderSide side = OrderSide::buy;
    OrderType type = OrderType::market;
    TimeInForce tif = TimeInForce::day;
    double qty = 0.0;           // shares (fractional allowed)
    double limitPrice = 0.0;    // for limit / stopLimit
    double stopPrice  = 0.0;    // for stop / stopLimit
    Timestamp createdAt;
    std::string strategyId;     // who emitted this
    std::string clientTag;      // free-form, surfaced in UI
};
```

### 4.2 Fill / Trade

```cpp
struct Fill {
    uint64_t orderId = 0;
    Timestamp ts;
    double qty = 0.0;
    double price = 0.0;
    double commission = 0.0;
    double slippage = 0.0;      // signed, positive = worse than mid
};

struct Trade {                  // a closed round-trip (entry + exit)
    std::string symbol;
    Timestamp openedAt;
    Timestamp closedAt;
    double qty = 0.0;
    double entryPrice = 0.0;
    double exitPrice  = 0.0;
    double commission = 0.0;
    double pnl = 0.0;           // signed, in account currency
    double pnlPct = 0.0;
    OrderSide direction = OrderSide::buy;   // direction of the *open* side
    std::string strategyId;
};
```

`Order` lives in `bte::core::Order`. `Fill` and `Trade` are the events the engine emits.

---

## 5. Portfolio

```cpp
struct Position {
    std::string symbol;
    double qty = 0.0;
    double avgCost = 0.0;       // VWAP across opens, after partial closes
};

struct PortfolioSnapshot {
    Timestamp ts;
    double cash = 0.0;
    std::vector<Position> positions;
    double realizedPnl = 0.0;
    double unrealizedPnl = 0.0;     // mark-to-market at this snapshot
    double equity() const { return cash + marketValue() + unrealizedPnl; /* see impl */ }
    double marketValue() const;     // sum(qty * lastPrice) — needs price oracle
};
```

The full `Portfolio` (mutable engine state) lives in `bte::engine` (`07`). `PortfolioSnapshot` is what crosses thread boundaries to the UI.

---

## 6. Result<T, Error>

```cpp
enum class ErrorCode {
    ok = 0,
    // generic
    invalidArgument,
    notFound,
    permissionDenied,
    cancelled,
    timeout,
    internal,
    // domain
    dataUnavailable,
    schemaMismatch,
    strategyCompileFailed,
    strategyRuntimeError,
    insufficientCash,
    insufficientShares,
    pluginIncompatibleAbi,
};

struct Error {
    ErrorCode code = ErrorCode::ok;
    std::string message;
    std::source_location where = std::source_location::current();
    std::vector<Error> causes;
    explicit operator bool() const { return code != ErrorCode::ok; }
};

template <typename T>
class Result {
public:
    Result(T v) : value_(std::move(v)) {}
    Result(Error e) : error_(std::move(e)) {}

    bool ok() const { return !error_; }
    const T& value() const& { return *value_; }
    T&&      value() && { return std::move(*value_); }
    const Error& error() const { return error_; }

private:
    std::optional<T> value_;
    Error error_;
};

// helpers
Error makeError(ErrorCode c, std::string msg,
                std::source_location loc = std::source_location::current());
```

Rules:
- Public API never throws. Internal helpers may throw `std::system_error` etc.; the boundary catches and wraps.
- `Error::message` is **already user-readable**. UI displays it directly.
- Logs always include `where.file_name():where.line()` and the cause chain.

---

## 7. Logging

`spdlog` with two sinks:
- Console (debug builds) — colorized.
- Rotating file at `<userData>/logs/stockBacktester.log` (10 MB × 5 files).

API:

```cpp
namespace bte::core::log {
spdlog::logger& main();             // category "bte"
spdlog::logger& engine();           // category "bte.engine"
spdlog::logger& data();             // category "bte.data"
// ... one per module
}
```

Level controlled by `settings.json` (`logLevel: "info"`).

---

## 8. Snapshots for cross-thread transport

To ship engine state to the UI without sharing pointers, we define **trivially copyable** snapshot structs:

```cpp
struct BarSnapshot       { /* same fields as Bar, plus barIndex */ };
struct TradeSnapshot     { /* same as Trade, plus indices */ };
struct PortfolioSnapshot { /* see §5 */ };
struct ReplayProgressSnapshot {
    int barIndex;
    int totalBars;
    Timestamp ts;
};
```

All are registered with `Q_DECLARE_METATYPE` in `bteBindings` and may travel across `Qt::QueuedConnection`. They have no shared ownership, so they're safe.

---

## 9. Money & precision

- **Use `double`** for prices and money. We're equities-only and never hold positions long enough for `double` accumulation error to matter. (Worst case: ~10 ppm error after 10⁹ ops; we're ~10⁵.)
- **Quantities** are `double` so fractional shares work uniformly.
- A future tick-precision bond / FX module would need fixed-point; we'll add `bte::core::Money` then. For now, `double` is documented and consistent.

---

## 10. Testing

- `Tests/Unit/Core/` with GoogleTest:
  - `Bar::isValid()` covers every OHLC violation case.
  - `Result<T,Error>` happy path + error path + chaining.
  - Time round-trip: `parseIso8601` → `toIso8601` is identity for any DuckDB-emitted timestamp.
  - `PortfolioSnapshot` equity invariants.

CI runs `ctest --preset dev` on every PR.
