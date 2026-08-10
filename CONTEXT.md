# Stock Backtesting

This glossary defines the project language used across strategy authoring, simulation, results, and replay.

## Language

**Backtest**:
A deterministic simulation of a Strategy over a fixed Release Snapshot and Run Configuration.
_Avoid_: Test run, simulation session

**Paced Backtest**:
A Backtest execution whose C++ engine advances under user or Debug Run pacing without changing event semantics.
_Avoid_: K-line Replay, second engine

**Strategy**:
The user-selected decision policy that observes a Market Slice and proposes orders without owning execution or accounting.
_Avoid_: Backtest engine, broker

**Selectable Conditions**:
A structured strategy authored through application controls and evaluated with explicit logical conditions.
_Avoid_: Rule script, visual Python

**Python Script Strategy**:
A trusted local Strategy authored in Python against the versioned strategy contract.
_Avoid_: Python engine, secure sandbox

**Market Slice**:
The synchronized set of actual bars available at one market timestamp.
_Avoid_: Tick, synthetic bar

**Run Configuration**:
The immutable choices that identify a Backtest, including its snapshot, universe, time range, capital, costs, and Strategy.
_Avoid_: Session state, settings

**Release Snapshot**:
A versioned, immutable market-data dataset built for an application release.
_Avoid_: Live database, user database

**Data Segment**:
A content-addressed portion of a Release Snapshot retained while a result references it.
_Avoid_: Result data copy

**Order**:
A Strategy request to trade that becomes eligible only under the engine's ordering and buying-power rules.
_Avoid_: Fill, trade

**Fill**:
The engine's immutable record that an eligible Order executed at a quantity, price, and time.
_Avoid_: Order, signal

**Position**:
The single net holding for one symbol, positive for long and negative for short.
_Avoid_: Lot, simultaneous long/short book

**Backtest Result**:
The durable record of a Backtest's configuration, events, diagnostics, and eligible metrics.
_Avoid_: Data snapshot, replay session

**K-line Replay**:
A time-ordered presentation of a Backtest Result against its referenced market bars.
_Avoid_: Paced Backtest, a second backtest engine
