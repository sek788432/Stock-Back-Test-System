# 07 — Engine, Replay, P&L

The engine is the heart of the backend. It pulls bars, runs the strategy, simulates fills, updates the portfolio, computes metrics, and feeds the UI.

It runs in **two execution modes** (distinct from strategy *authoring* modes in `05`/`11`) with the **same code path**:

- **Backtest** — drains a `BarStream` as fast as possible.
- **Replay** — **K-line replay**: paces the identical pipeline with a `ReplayClock` so the UI animates candles, volume, and trade markers (`02` §2.3, `11` §1).

**Session inputs.** Each replay attaches to **`BarStream`** configuration `(symbol, schema/timeframe, date range[, initialCapital from EngineConfig])`. Missing history or mismatched schemas surface as structured `Result` errors in the bindings layer (`04`).

---

## 1. Engine config

```cpp
namespace bte::engine {

struct EngineConfig {
    double initialCash = 100'000.0;

    // Broker / fill model
    enum class FillModel { nextBarOpen, currentBarClose, midpoint, custom };
    FillModel fillModel = FillModel::nextBarOpen;

    double commissionPerShare = 0.0;
    double commissionPctNotional = 0.0005;          // 5 bps
    double slippageBps = 1.0;                        // 1 bp adverse on every fill
    bool   allowShort = false;
    bool   allowFractional = true;
    double minNotional = 1.0;                        // ignore micro orders

    // Risk / safety rails
    double maxLeverage = 1.0;                        // < 1.0 = cash-only
    double maxOrderPctEquity = 1.0;                  // reject larger orders
    bool   rejectOnInsufficientCash = true;          // else: shrink to fit

    // Determinism / logging
    bool stopOnStrategyError = true;
    int  checkpointEveryNBars = 1000;                // replay scrub support
};

}  // namespace bte::engine
```

Defaults match a sensible US equities long-only setup. Persisted in `<userData>/config/settings.json`.

---

## 2. Broker simulator

The broker is the only thing that turns `Order` → `Fill`. It is **deterministic** given the bar stream.

### 2.1 Fill timing

- `nextBarOpen` (default, recommended) — orders submitted on bar `t` fill at the open of bar `t+1`. This avoids look-ahead bias.
- `currentBarClose` — fill at the same bar's close. Fastest for backtests; introduces minor look-ahead and is documented as such.
- `midpoint` — fill at `(high + low) / 2` of the next bar. Smoother but optimistic.
- `custom` — plugin callback (see `08`).

### 2.2 Slippage

For market and stop orders the executed price is:

```
fillPrice = referencePrice * (1 + side * slippageBps / 10_000)
```

where `side = +1 for buy, -1 for sell`, so slippage always works against the trader.

### 2.3 Limit / stop logic

A limit buy at price `L`:
- If next bar's `low <= L`, fill at `min(open, L)`.
- Else carry to the next bar (TIF `day` cancels at session boundary; we approximate this with bar-day boundary).

A stop sell at price `S`:
- If next bar's `low <= S`, trigger at `S`, then market-fill at `min(open, S) * (1 - slippage)`.

These rules sit inside `BrokerSimulator::onBar(prevOrders, nextBar)` and are < 200 lines of code.

### 2.4 Commission

```
commission = qty * commissionPerShare + |notional| * commissionPctNotional
```

Both legs (buy and sell) charge commission. Subtracted from cash on fill.

---

## 3. Portfolio mechanics

```cpp
class Portfolio {
public:
    explicit Portfolio(double initialCash);

    core::Result<void> applyFill(const core::Fill& fill,
                                  const core::Order& order);
    void mark(const std::map<std::string, double>& lastPrices);

    double cash() const;
    double realizedPnl() const;
    double unrealizedPnl() const;
    double equity() const;
    const Position* positionFor(std::string_view sym) const;

    core::PortfolioSnapshot snapshot(core::Timestamp ts) const;

private:
    double cash_;
    std::unordered_map<std::string, Position> positions_;
    double realizedPnl_ = 0.0;
    std::map<std::string, double> lastPrices_;       // for mark-to-market
};
```

- **Average cost** updated as VWAP across opens. Partial closes do **not** re-cost remaining shares.
- **Realized P&L** = `(exitPx - avgCost) * closedQty - commission` accumulated across closes.
- **Unrealized P&L** = `Σ (lastPrice - avgCost) * qty` recomputed on `mark()`.
- **Equity** = `cash + Σ qty * lastPrice` (which equals `cash + costBasis + unrealized`).

Position state mutates only via `applyFill`. Mark prices come in via `mark()` once per bar from the engine — that is the only place lastPrice changes.

---

## 4. Backtest engine

### 4.1 Loop

```cpp
core::Result<BacktestResult> run(BarStream& stream,
                                  IStrategy& strategy,
                                  const EngineConfig& cfg,
                                  std::stop_token stop) {
    Portfolio portfolio(cfg.initialCash);
    BrokerSimulator broker(cfg);
    Indicators indicators;

    InitContext init{indicators, /*...*/};
    if (auto r = strategy.onInit(init); !r.ok()) return r.error();

    int barIndex = 0;
    std::optional<core::Bar> prev;

    while (auto bar = stream.next()) {
        if (stop.stop_requested()) return Error{ErrorCode::cancelled, "user cancelled"};

        // 1. Fill yesterday's pending orders against this bar's open
        auto fills = broker.onNewBar(*bar);
        for (const auto& fill : fills) {
            (void)portfolio.applyFill(fill, broker.orderById(fill.orderId));
            strategy.onFill(fill, /*ctx=*/ ...);
        }

        // 2. Update indicators with this bar
        indicators.update(*bar);

        // 3. Mark portfolio at this bar's close
        portfolio.mark({{stream.symbol(), bar->close}});

        // 4. Run strategy
        Context ctx{*bar, barIndex, indicators, portfolio.view(), broker.builder()};
        if (auto r = strategy.onBar(ctx); !r.ok()) {
            if (cfg.stopOnStrategyError) return r.error();
            log::engine().error("strategy error: {}", r.error().message);
        }

        // 5. Validate, queue, and (later) emit fills for next bar
        broker.acceptOrders(builder.takeOrders());

        // 6. Snapshots / progress
        if (cfg.checkpointEveryNBars > 0 && barIndex % cfg.checkpointEveryNBars == 0) {
            checkpoints.push_back(portfolio.checkpoint());
        }

        prev = *bar;
        ++barIndex;
    }

    strategy.onShutdown();
    return assembleResult(portfolio, broker.tradeLog(), checkpoints);
}
```

The loop is single-threaded by design — strategies see deterministic order. Parallel **multi-strategy** runs are achieved by running multiple engines in parallel on different worker threads, each owning its own `BarStream` connection to DuckDB.

### 4.2 `BacktestResult`

```cpp
struct BacktestResult {
    EngineConfig config;
    std::string symbol;
    core::DateRange range;
    std::vector<core::Trade> trades;
    std::vector<EquityPoint> equityCurve;          // (ts, equity) per bar
    std::vector<core::Fill> fills;
    Metrics metrics;                                // computed by bteMetrics
    std::vector<PortfolioCheckpoint> checkpoints;   // for replay scrub
};
```

---

## 5. Replay engine

Same pipeline, gated by a clock.

```cpp
class Replay {
public:
    Replay(std::unique_ptr<BarStream>, std::shared_ptr<IStrategy>, EngineConfig);

    void play();
    void pause();
    void step();                                     // advance exactly 1 bar
    void setSpeedMultiplier(double mult);            // 1.0 = realtime; 0 = max speed
    void seek(int barIndex);                          // jump using checkpoints
    ReplayState state() const;

    // Subscribe to per-bar updates (engine thread emits via callback;
    // bindings translate to Qt signals)
    void onBar(std::function<void(const core::Bar&, int barIndex)>);
    void onTrade(std::function<void(const core::Trade&)>);
    void onPortfolio(std::function<void(const core::PortfolioSnapshot&)>);
};
```

### 5.1 `ReplayClock`

```cpp
struct ReplayClock {
    double speedMultiplier = 1.0;     // 0 = no wait
    std::chrono::milliseconds intervalAtOneX{1000};

    void waitForNext() {
        if (speedMultiplier == 0) return;
        std::this_thread::sleep_for(intervalAtOneX / speedMultiplier);
    }
};
```

The UI maps speed presets to `speedMultiplier` and `intervalAtOneX`:
- 1× → 1 bar/sec
- 5× → 5 bars/sec
- 10× → 10 bars/sec
- max → 0 (no wait)

### 5.2 Scrubbing

Naive approach: replay from start each scrub. With 25k bars at 1 µs/bar that's 25 ms — fine for typical backtests, but we still want fast-feeling drag-scrub.

The engine maintains **portfolio checkpoints** every `checkpointEveryNBars` (default 1000): `(barIndex, Portfolio, Indicators state, Strategy state)`. Each checkpoint is ~few KB. To seek to bar `N`:

1. Find the latest checkpoint `≤ N`.
2. Restore engine state from it.
3. Re-run forward from checkpoint to `N` (≤ 1000 bars).

`Indicators::checkpoint()` and `Strategy::checkpoint()` (Lua: serialize via `bte.snapshot()` user-extension; Rule mode: rule engine has no internal state — just snapshot is `barIndex`).

For initial release, we **only checkpoint the Portfolio**, and re-warm indicators by replaying from the checkpoint. That's acceptable since indicators are O(1)/bar.

### 5.3 Cancellation & teardown

- `pause()` flips a flag the loop reads after each bar.
- Closing the Replay tab calls `Replay::~Replay()`, which signals stop and joins the worker. Bounded teardown < 100 ms.

---

## 6. Metrics (`bteMetrics`)

```cpp
struct Metrics {
    double totalReturn;
    double cagr;
    double volatilityAnnualized;
    double sharpe;
    double sortino;
    double maxDrawdown;        // signed, e.g. -0.15
    double maxDrawdownDuration; // bars
    double calmar;
    double winRate;             // 0..1
    double avgWin;
    double avgLoss;
    double payoffRatio;
    double expectancy;
    int    tradeCount;
    double exposureFraction;   // % of bars with a position
};

Metrics computeMetrics(const std::vector<EquityPoint>& curve,
                       const std::vector<core::Trade>& trades,
                       std::optional<double> riskFreeAnnual = 0.0);
```

Annualization assumes:
- `ohlcv-1d` → 252 bars/yr
- `ohlcv-1h` → 252 * 6.5 ≈ 1638 bars/yr
- `ohlcv-1m` → 252 * 6.5 * 60 ≈ 98280 bars/yr

The mapping lives in `bteMetrics::barsPerYear(schemaName)`. Custom schemas pass an explicit `barsPerYear`.

---

## 7. Multi-symbol / basket runs

`BasketReplay` wraps a `MergedBarStream` (`04`) and feeds bars symbol-by-symbol per timestamp. Strategy `onBar` receives a `Context` whose `bar` carries `symbol`. Portfolio handles many positions naturally.

In Phase 1 the UI exposes only single-symbol replay; the engine API is already basket-capable so Phase 2 is "wire up the UI", no engine refactor.

---

## 8. Determinism guarantees

For any combination of:
- same `BarStream` data
- same strategy file (rule JSON, Lua, or Python once enabled)
- same `EngineConfig`
- same engine version

The engine produces identical:
- `trades` (down to floating-point bits)
- `equityCurve`
- `metrics`

This is **tested**: a fixture run yields a JSON dump; CI compares to a pinned reference.

---

## 9. Performance targets

| Workload | Target | Where |
|---|---|---|
| Backtest 25k bars × 1 symbol, simple SMA cross | < 50 ms | release build, M1 / Ryzen 5 |
| Backtest 25k bars × 500 symbols (sequential) | < 30 s | same |
| Replay 1× speed | < 1% CPU on average | trivial |
| Replay max speed | ≥ 5000 bars/sec | piping rate to UI |
| Scrub 25k bars to arbitrary point | < 200 ms | uses checkpoints |

Bench harness in `Tests/Bench/` using `nanobench` (single header).

---

## 10. Tests

- Determinism test (described in §8).
- Broker model: every `FillModel` has a fixture with hand-checked expected fills.
- Stop / limit triggers: edge cases (gap-down through stop, gap-up through limit) verified.
- Insufficient cash: configurable rejection vs shrink.
- Cancellation: `stop_token` set mid-run yields graceful exit < 100 ms.
- Checkpoint round-trip: backtest with checkpoints every 100 bars; reconstruct equity at every point matches direct run.
