# 0002 — C++ and Qt as the desktop stack

- **Status**: Accepted
- **Date**: 2026-05-06
- **Deciders**: founding team
- **Supersedes**: —
- **Superseded by**: —

## Context

We need a cross-platform (Windows / macOS / Linux) desktop application for stock backtesting and replay. The data pipeline already in this repo is Python; the question is what to build the desktop on.

Constraints:

- Must run on all three OSes from one codebase.
- Performance matters in the engine bar loop and replay tick (Specs/07 §9).
- One developer (initially) is going to maintain everything; framework maintenance burden has to be small.
- Must integrate with DuckDB for fast historical reads.
- License compatibility for eventual distribution (private team today, but options should remain open).

## Decision

- **Language**: C++20 for the desktop application.
- **UI framework**: Qt 6 LTS (currently 6.8) using the **Widgets** module + **Qt Charts** for the candlestick/equity views.
- **Build**: CMake 3.24+ with `CMakePresets.json` and vcpkg manifest for dependency pinning.
- **Backend code**: pure C++20, isolated from Qt; bridge classes (`bteBindings`) translate to `Q_OBJECT` view-models for the UI.

Details and module breakdown are in [`../Specs/01Architecture.md`](../Specs/01Architecture.md) and [`../Specs/02FrontendQt.md`](../Specs/02FrontendQt.md).

## Consequences

**Positive:**

- One toolchain across all three OSes.
- Qt's deployment tooling (`windeployqt`, `macdeployqt`, `linuxdeploy`) is mature; cross-platform packaging is a known problem (Specs/09).
- Native look-and-feel on each OS (Widgets); `QCandlestickSeries` lets us start with candles for free.
- Engine code is plain C++ and unit-testable without instantiating Qt.
- LGPL/commercial licensing of Qt matches our flexibility goals (no GPL contagion).

**Negative:**

- Qt has a learning curve and a sizable install footprint (~100 MB per platform, packaged).
- Widgets is more verbose than QML; more code to write per UI surface.
- Qt 6's CMake integration assumes CMake ≥ 3.24, raising the floor on contributor environments.

**Mitigations:**

- The MVVM split (Specs/02 §3) keeps non-UI logic out of Qt classes, so unit tests don't need Qt.
- The `cpp-modern-style` and `cpp-oop-design` skills steer authors away from Qt-isms in backend code.
- vcpkg manifest pins Qt and friends to known versions so contributor envs don't drift.

## Alternatives considered

1. **Tauri / Electron + web frontend**. Rejected: the engine is performance-sensitive C++ either way; piping bars over IPC to a web UI adds latency for replay; Electron's memory footprint is non-trivial for a single-user desktop app.
2. **Qt Quick / QML instead of Widgets**. Considered: better animation story, more modern look. Rejected for this team because the app is form-heavy (strategy editor, settings, dashboards), QML adds a JS layer to maintain, and Widgets gives native-feeling menus and dialogs without theming work.
3. **wxWidgets**. Rejected: smaller community, weaker tooling, no charting equivalent.
4. **GTK4 / GNOME Builder**. Rejected: weak macOS and Windows story.
5. **C# + Avalonia**. Rejected: introduces a second runtime, .NET deployment story across three OSes is uneven, no clear win over Qt for a single dev.
6. **C++ with a custom OpenGL/Skia UI**. Rejected: months of work for what Qt gives in days.
7. **Rust + egui / Slint**. Considered. Rejected for now: smaller ecosystems for charting and accessibility; team's C++ familiarity is the dominant factor. Worth re-evaluating in 1–2 years.

## Charting library — sub-decision

Within Qt, we picked **Qt Charts** over QCustomPlot or a custom QPainter renderer. The full reasoning is in [`../Specs/02FrontendQt.md`](../Specs/02FrontendQt.md) §1; the headline is licensing simplicity (no separate GPLv3 from QCustomPlot) and zero new third-party tracking.

The chart layer is hidden behind `IChartView` (Specs/02 §4) so swapping to QCustomPlot or custom QPainter later is a contained change, should performance or styling demand it.

## References

- [`../Specs/01Architecture.md`](../Specs/01Architecture.md)
- [`../Specs/02FrontendQt.md`](../Specs/02FrontendQt.md)
- [`../Specs/09BuildDistributionLauncher.md`](../Specs/09BuildDistributionLauncher.md)
