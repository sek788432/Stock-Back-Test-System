# 02 — Frontend (Qt)

The Qt application: every screen the user sees, the chart engine for replay, and how the UI talks to the backend without ever blocking.

---

## 1. Framework choice — Qt 6 LTS, Widgets + Qt Charts

We use **Qt 6 LTS (currently 6.8)** with the **Widgets** module for layout and **Qt Charts** for candlesticks and the equity curve.

### Why Widgets, not QML?

| Criterion | Widgets | QML / Qt Quick |
|---|---|---|
| Native look on desktop (titlebars, menus, file dialogs) | ★★★★★ | ★★★ (needs styling) |
| Engineer ramp-up if you already know C++ | ★★★★★ | ★★★ (JS + property bindings) |
| Tooling on all 3 OSes (Designer, Creator) | mature | mature |
| Performance for tabular data and forms | ★★★★★ | ★★★★ |
| Performance for animated charts at 60 fps | ★★★★ (with Qt Charts + GL backend) | ★★★★★ |
| Easy maintenance for one developer | ★★★★★ | ★★★ (JS + C++ split is more files) |

For a single-developer desktop tool that is form-heavy (strategy editor, dashboards, dialogs) and has one animated view (replay), **Widgets wins on maintenance**. We use Qt Charts inside a `QChartView` for the chart — that one view is fast enough.

### Why Qt Charts (not QCustomPlot, not custom QPainter)?

You asked for "good-looking and easy to maintain". Trade-offs:

| Library | Look | Performance | License | Maintenance |
|---|---|---|---|---|
| **Qt Charts** (chosen) | Modern, themeable, antialiased, dark-mode aware out of the box. Built-in `QCandlestickSeries`, `QLineSeries`, axes, legends. | Good up to ~5–10k visible points; we **only ever render the visible window** (200–500 candles), so this is a non-issue. | LGPL/Commercial — same license as Qt itself. **No new license to manage.** | Same release cadence as Qt LTS; if Qt is supported, Charts is supported. |
| QCustomPlot | Engineering look, less "modern". Excellent. | Excellent — single-header, tens of thousands of points without breaking a sweat. | **GPLv3 or paid commercial.** This is a real liability if you ever want to ship closed-source. | Single maintainer; very mature but third-party. |
| Custom QPainter / Qt Quick Scene Graph | Anything you can paint. | As good as you make it. | Inherits Qt. | **Months of work** for proper trading-grade rendering (axes, crosshair, scaling, perf). Not worth it now. |

**Decision: Qt Charts.** The reasoning is licensing simplicity (no new GPL constraint), zero third-party dependency to track, and the performance ceiling is irrelevant when we always windowed-render. If we ever need pro-grade tick replay (1-minute bars over 10 years all visible), we revisit; the chart layer is hidden behind a `IChartView` interface (see §4) so swapping is a contained change.

We layer simple visual polish on top:
- Custom `QPalette` with two named themes (Light / Dark) loaded from QSS.
- Crosshair, candle hover tooltip, and trade markers drawn as a `QGraphicsItem` overlay on top of `QChartView` — Qt Charts exposes `mapToValue` so this is straightforward.

---

## 2. Window structure

```
┌───────────────────────────────── stockBacktester ─────────────────────┐
│ File   Strategy   Data   View   Help                                  │
├───────────────────────────────────────────────────────────────────────┤
│ ┌─ Tabs ──────────────────────────────────────────────────────────┐   │
│ │ [Strategies] [Backtest] [Replay] [Screener] [Plugins] [Logs]    │   │
│ ├─────────────────────────────────────────────────────────────────┤   │
│ │                                                                 │   │
│ │                  (active tab content)                           │   │
│ │                                                                 │   │
│ └─────────────────────────────────────────────────────────────────┘   │
│ status bar  │  symbol: AAPL  │  schema: ohlcv-1h  │  rows: 25,431     │
└───────────────────────────────────────────────────────────────────────┘
```

### 2.1 Strategies tab

A two-pane view:
- **Left** — list of saved strategies (from `<userData>/strategies/`).
- **Right** — editor for the selected strategy.

Editor has **four visible modes** (three concrete artifact types):

1. **Built-in components (rule)** (default) — form-driven rows (indicator, filter, portfolio gate…) with **`ALL conditions` / `ANY condition`** logic matching `conditionLogic` in `05` §3. Generates `*.rule.json`.
2. **Python** — code editor with highlighting, diagnostics debounce (`05` §5), **Validate** invoking `compilePython()`.
3. **Natural language (AI)** — prompt + chat transcript panes; generated text **must not** auto-run. **Accept** copies into Python or rule mode for compilation (`05` §6, `11` §3).
4. **Lua** (optional / advanced) — retained for compatibility and reference strategies until Python reaches parity — same patterns as today (`05` §4).

Bottom of the editor has **Run Backtest** and **Open in Replay** buttons.

### 2.2 Backtest tab

| Region | Content |
|---|---|
| Top toolbar | Symbol picker, date range, schema dropdown (read from DuckDB), commission/slippage settings, **Run** button. |
| Center | Equity curve (`QLineSeries`) + benchmark (buy-and-hold) overlay. |
| Right side panel | Summary metrics: total return, CAGR, Sharpe, max drawdown, win rate, # trades, exposure. |
| Bottom | Sortable trade log table (`QTableView` + `QSortFilterProxyModel`). |

**Cross-link.** A dedicated CTA (toolbar or footer button) SHOULD deep-link results into Replay so users can reconcile fills on the candlestick chart (`11` §1).

### 2.3 Replay tab — K-line setup

Treat **Replay** as the **K-line (candlestick) playback surface** keyed by **`(symbol, timeframe/schema, inclusive date range, initial capital)`**, matching `11` §1 inputs. Playback chrome (speed presets, scrub, portfolio strip, trade markers) inherits `ReplaySessionVm`.

The headline UX. Layout:

```
┌─────────────────────────────────────────────────────────────────────┐
│  Symbol [AAPL ▼]   Speed [1× 5× 10× max]   ◀◀ ⏸ ▶ ▶▶   ███████░░░  │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│        ▲                                                            │
│        │       ╷  ╷  ╶─                                             │
│        │   ╷╷  │  │     ▼ Sell @ 187.20                             │
│        │  ╶┤│ ╶┤  │                                                 │
│        │   ││  │  │     ▲ Buy  @ 173.40                             │
│        │   │   │  │                                                 │
│        └──────────────────────────────────────────────────►         │
│                                                                     │
├──────────────────┬──────────────────────────────────────────────────┤
│  Cash:  $9,830   │   Equity curve so far  ╱╲╱╲╱╲                    │
│  Held:  20 AAPL  │                                                  │
│  Mkt:   $3,744   │   Realized P/L:  +$214.30                        │
│  Total: $13,574  │   Unrealized:    -$ 12.10                        │
└──────────────────┴──────────────────────────────────────────────────┘
```

Interactions:
- **Speed control** — 1× = 1 bar / sec real-time; max = drain as fast as the engine can.
- **Pause / Step** — `Space` to pause/resume, `→` to step one bar.
- **Scrub bar** — drag to a moment in time; engine rebuilds portfolio state by replaying from start (we keep checkpoints every 1000 bars to make this fast — see `07`).
- **Trade markers** — green up-triangle for buys, red down-triangle for sells, click to see fill details.
- **Cursor inspector** — hover any candle to see OHLCV + active indicators in a side popup.
- **Volume pane** — histogram under the candles, sharing the categorical axis window (`11` §1).

Universe picker + predicate builder mirrors the strategy editor modalities (`11` §2):

| Region | Content |
| --- | --- |
| Top | Tabs: **Technical / Fundamental (when data exists) / Preset / Custom (Python) / AI (NL)** — exact availability depends on data pipeline staging. |
| Builder | Reuses the same row model as rule mode for built-ins; separate Python editor for scripted scans; NL assistant identical to strategy flow. |
| Logic | **Match ALL** vs **Match ANY** selector for built-in rows (maps to screener `conditionLogic`). |
| Actions | **Run Screener**, export (CSV/clipboard). |
| Results | `QTableView` with rank, symbol, company, price, change %, market cap, sector — plus optional performance/sector summary views in later milestones. |

### 2.5 Plugins tab

Read `<userData>/plugins/`, list each `.so` / `.dll` / `.dylib` with:
- Plugin name, version, author (from `bteGetPluginManifest`)
- Strategies and indicators it registered
- Enable/disable toggle
- "Open folder" button

### 2.6 Logs tab

Tails `<userData>/logs/stockBacktester.log` via `QFileSystemWatcher` + ring buffer in memory, color-coded by level.

---

## 3. View-Model pattern

We use a **lightweight MVVM**:

- **Model** — backend types (`Bar`, `Trade`, `BacktestResult`). Pure C++.
- **ViewModel** — `Q_OBJECT` adapter living in `bteBindings`. Owns a backend object, exposes `Q_PROPERTY` and signals.
- **View** — `QWidget` subclass. Subscribes to ViewModel signals. Never holds backend pointers.

Example:

```cpp
namespace bte::bindings {

class ReplaySessionVm : public QObject {
    Q_OBJECT
    Q_PROPERTY(double cash READ cash NOTIFY portfolioChanged)
    Q_PROPERTY(double equity READ equity NOTIFY portfolioChanged)
    Q_PROPERTY(int barsProcessed READ barsProcessed NOTIFY tick)
    Q_PROPERTY(ReplayState state READ state NOTIFY stateChanged)

public:
    explicit ReplaySessionVm(std::shared_ptr<engine::Replay> replay,
                             QObject* parent = nullptr);

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void step();
    Q_INVOKABLE void setSpeedMultiplier(double mult);
    Q_INVOKABLE void seekToBar(int barIndex);

signals:
    void tick(int barIndex, BarSnapshot bar);     // queued from worker
    void tradeFilled(TradeSnapshot trade);
    void portfolioChanged(PortfolioSnapshot snap);
    void stateChanged(ReplayState newState);
    void errorOccurred(QString message);

private:
    std::shared_ptr<engine::Replay> replayImpl_;
    QThread worker_;
};

}  // namespace bte::bindings
```

`BarSnapshot`, `TradeSnapshot`, `PortfolioSnapshot` are **trivial copyable** types registered with `Q_DECLARE_METATYPE` so they cross the queued-signal boundary safely.

---

## 4. Chart abstraction

```cpp
class IChartView {
public:
    virtual ~IChartView() = default;
    virtual void setBarWindow(std::span<const Bar> visible) = 0;
    virtual void appendBar(const Bar& bar) = 0;          // streaming during replay
    virtual void addIndicatorOverlay(const std::string& name,
                                     std::span<const double> values) = 0;
    virtual void addTradeMarker(const TradeMarker& m) = 0;
    virtual void clearMarkers() = 0;
    virtual void setCrosshair(std::optional<int> barIndex) = 0;
};
```

Concrete impl: `QtChartsCandlestickView : QWidget, IChartView`. Swapping to QCustomPlot later is one new file.

---

## 5. Persistence

| What | Where | Format |
|---|---|---|
| Strategies | `<userData>/strategies/<name>.{rule.json,lua,py}` | JSON (rule mode) or scripts + optional `.meta.json` |
| Screener presets | `<userData>/screeners/<name>.json` | Stores universe, `conditionLogic`, rows, optional Python path |
| Saved replay sessions | `<userData>/sessions/<timestamp>.json` | JSON: symbol, range, strategy ref, last-bar position |
| Settings | `<userData>/config/settings.json` | JSON: theme, db path, default schema, commission defaults |
| Last layout | `<userData>/config/window.bin` | `QByteArray` from `saveState()` |

All paths resolve through `bteCore::userDataDir()` so the test suite can redirect them.

---

## 6. Theming

- Two QSS files in `Resources/Themes/{light.qss,dark.qss}`.
- Loaded by `QApplication::setStyleSheet` based on `settings.json`.
- Qt Charts theme set via `QChart::setTheme(QChart::ChartThemeDark)` to match.

---

## 7. Internationalization

Wrap all user-visible strings in `tr(...)`. Keep a `Resources/i18n/stockBacktester_en.ts` baseline. We won't ship translations day one, but the wrapping makes them cheap to add.

---

## 8. Accessibility

- Every widget has `accessibleName`.
- Every action has a keyboard shortcut. Replay tab shortcuts: `Space` play/pause, `→` step, `←` step back, `1`/`5`/`9`/`0` set speed.
- Color choices for buy/sell markers also use shape (▲/▼) so colorblind users still distinguish.

---

## 9. Tests

- `Tests/Unit/Frontend/` uses Qt Test:
  - Smoke test: open each tab, ensure no crashes.
  - Replay state machine: simulate signals, assert UI labels update.
  - Strategy editor: invalid Lua or Python stub → underline + compile error surfaced in the diagnostics strip. **NL** smoke: **Run Backtest** stays disabled until the user **Accept**s generated text into an editor **and** `compile*` succeeds (`05` §6).
- Visual regression on charts is **out of scope** for now; we eyeball it.
