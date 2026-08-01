# 06 — Indicators API

The technical-indicator library (`bteIndicators`). Every indicator is **incremental** (one bar in, one value out, O(1) amortized) so we never re-compute history during replay.

---

## 1. Design principles

1. **Streaming-first.** Each indicator stores just enough state to update on the next bar. No "give me 5 years of bars and I'll spit out 5 years of values" — we feed bar-by-bar.
2. **Pure functions of state + bar.** No globals, no I/O.
3. **Warm-up tracked explicitly.** Until enough bars have arrived, `value()` returns `std::nullopt`. The strategy must guard.
4. **No exceptions.** Bad params caught at construction (`Result<>` factory).
5. **Composable.** Indicators can take other indicators as inputs (`std::shared_ptr<IIndicator>`).

---

## 2. Base interface

```cpp
namespace bte::indicators {

class IIndicator {
public:
    virtual ~IIndicator() = default;

    // Called by the engine for every new closed bar (in order).
    virtual void update(const core::Bar& bar) = 0;

    // Latest value; nullopt if still warming up.
    virtual std::optional<double> value() const = 0;

    // History of recent values; size depends on indicator. Always aligned with bars.
    virtual std::span<const double> history() const = 0;

    // Bars since construction.
    virtual int64_t consumed() const = 0;

    // Identity for caching, logging, charting.
    virtual std::string id() const = 0;
    virtual std::string kind() const = 0;          // "sma", "rsi", ...

    // Reset to fresh state (used for replay scrubbing).
    virtual void reset() = 0;
};

}  // namespace bte::indicators
```

Some indicators emit **vectors** per bar (MACD = line + signal + histogram). They derive from a multi-output base:

```cpp
class IMultiIndicator : public IIndicator {
public:
    virtual std::optional<double> valueAt(std::string_view channel) const = 0;
    virtual std::vector<std::string> channels() const = 0;
};
```

---

## 3. Built-in catalog (Phase 1)

| `kind` | Args | Outputs | Notes |
|---|---|---|---|
| `sma` | `period: int` | scalar | Simple moving average over close. |
| `ema` | `period: int` | scalar | α = 2 / (period + 1). |
| `wma` | `period: int` | scalar | Weighted (linear) MA. |
| `rsi` | `period: int = 14` | scalar | Wilder smoothing. |
| `macd` | `fast=12, slow=26, signal=9` | `line`, `signal`, `hist` | EMA-based, standard. |
| `bb` (Bollinger) | `period=20, k=2` | `upper`, `mid`, `lower`, `width`, `pctB` | k = std-dev multiplier. |
| `atr` | `period=14` | scalar | Wilder smoothing of true range. |
| `adx` | `period=14` | `adx`, `plusDi`, `minusDi` | Wilder. |
| `stoch` | `kPeriod=14, dPeriod=3` | `k`, `d` | %K and %D. |
| `donchian` | `period=20` | `upper`, `lower`, `mid` | Channel highs/lows. |
| `vwap` | `windowBars: int? (null = session)` | scalar | Daily reset if `windowBars` is null. |
| `obv` | — | scalar | On-balance volume. |
| `roc` | `period=10` | scalar | Rate of change %. |
| `momentum` | `period=10` | scalar | Close - close[period]. |
| `tr` | — | scalar | True range (input to ATR). |
| `crossover` | `a, b: indicator` | bool/scalar | +1 / -1 / 0 cross signal — see §5. |
| `passthrough` | `field: open|high|low|close|volume` | scalar | Wraps a bar field as an indicator (so rule / script modes can compose). |

**Phase 2 (room reserved, not implemented day 1):** `keltner`, `cci`, `mfi`, `parabolicSar`, `ichimoku`, custom user indicators via plugins (see `08`).

---

## 4. Construction

Two ways:

```cpp
// 1. Direct C++ usage (engine internals)
auto rsi = std::make_unique<RsiIndicator>(/*period*/14);

// 2. By name + JSON args (used by rule/compiler, Lua, Python host, plugins)
auto handle = registry.create("rsi", { {"period", 14} });
```

Registry:

```cpp
class IndicatorRegistry {
public:
    using Factory = std::function<core::Result<std::unique_ptr<IIndicator>>(const ArgMap&)>;

    void registerKind(std::string kind, Factory f);
    core::Result<std::unique_ptr<IIndicator>> create(std::string_view kind,
                                                     const ArgMap& args) const;

    static IndicatorRegistry& builtin();   // populated at static-init with the catalog above
};
```

Plugins call `registerKind` to add custom indicators (see `08`).

---

## 5. Cross helpers

Used heavily by strategies. Implemented as a small stateful wrapper rather than ad-hoc per-strategy logic:

```cpp
// returns the *signed* cross signal on each bar, after both indicators have values:
//   +1 if `a` crossed above `b` this bar
//   -1 if `a` crossed below `b` this bar
//    0 otherwise
struct CrossDetector {
    void update(double a, double b);
    int signal() const;     // -1, 0, +1
    bool justCrossedAbove() const { return signal() > 0; }
    bool justCrossedBelow() const { return signal() < 0; }
};
```

Rule mode (`crosses.above`), Lua (`bte.crossesAbove`), and Python strategies (same detector exposed through the strategy binding layer per **`05`**) all share this implementation per monitored pair.

---

## 6. Memory & history depth

By default each scalar indicator keeps the **last 4096 values** in a ring buffer. The engine can override via `setHistoryDepth(n)`. The chart UI reads `history()` to draw indicator overlays — we never query the engine for "value at bar N" outside the visible window.

If a user strategy needs longer lookbacks (e.g. 200-bar SMA), the indicator must internally store at least its required window. The 4096 history limit is for **display**, not correctness.

---

## 7. Numerical precision

- All accumulators use `double`.
- For `sma` and `wma` we use a **rolling sum** technique (subtract old, add new) — fast but accumulates floating-point drift. Mitigation: every 4096 bars we recompute from the ring's actual values. Drift before recompute is < 1e-10 of the sum for our price ranges.
- For `ema` we use the standard recursive form, which is self-correcting (no recomputation needed).
- `rsi` and `atr` use Wilder's smoothing in canonical form (`prev * (n-1)/n + x/n`).

---

## 8. Example — RSI implementation sketch

```cpp
class RsiIndicator : public IIndicator {
public:
    static core::Result<std::unique_ptr<IIndicator>> create(int period);

    void update(const core::Bar& bar) override {
        const double close = bar.close;
        if (consumed_ == 0) { prevClose_ = close; ++consumed_; return; }

        const double delta = close - prevClose_;
        const double gain = std::max(delta, 0.0);
        const double loss = std::max(-delta, 0.0);

        if (consumed_ <= period_) {
            sumGain_ += gain;
            sumLoss_ += loss;
            if (consumed_ == period_) {
                avgGain_ = sumGain_ / period_;
                avgLoss_ = sumLoss_ / period_;
                value_ = computeRsi(avgGain_, avgLoss_);
            }
        } else {
            avgGain_ = (avgGain_ * (period_ - 1) + gain) / period_;
            avgLoss_ = (avgLoss_ * (period_ - 1) + loss) / period_;
            value_ = computeRsi(avgGain_, avgLoss_);
        }
        prevClose_ = close;
        history_.push(value_);
        ++consumed_;
    }

    std::optional<double> value() const override { return value_; }
    // ...

private:
    int period_;
    double prevClose_ = 0.0, sumGain_ = 0.0, sumLoss_ = 0.0;
    double avgGain_ = 0.0, avgLoss_ = 0.0;
    std::optional<double> value_;
    int64_t consumed_ = 0;
    Ring<double> history_{4096};
};
```

Other indicators follow the same skeleton.

---

## 9. Testing strategy

- **Reference values**: each indicator has a CSV fixture (`Tests/Unit/Indicators/Fixtures/Rsi.csv`) generated once with `pandas-ta`. Tests assert `|c++ value - reference| < 1e-9` per bar.
- **Reset correctness**: feed N bars, snapshot value; reset; feed same N bars; assert value matches.
- **Composition**: `BollingerBands` constructed with a custom SMA produces the same numbers as the default.
- **Allocation**: `update()` must not allocate after first 4096 bars. Verified with a custom allocator counting calls.

---

## 10. Charting integration

When the UI needs to overlay an indicator on the candlestick view, it calls:

```cpp
struct IndicatorSeries {
    std::string name;          // "SMA(20)"
    std::vector<double> values; // aligned with visible bar window, NaN for warm-up
};

IndicatorSeries snapshotForWindow(const IIndicator& ind, int firstBarIndex, int count);
```

`snapshotForWindow` reads from `history()` and pads with `NaN` where the indicator hadn't warmed up yet. The chart uses `NaN` as "skip" so lines don't draw through warm-up.
