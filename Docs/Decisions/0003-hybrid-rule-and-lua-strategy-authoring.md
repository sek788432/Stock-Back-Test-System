# 0003 — Hybrid rule + Lua strategy authoring

- **Status**: Accepted
- **Date**: 2026-05-06
- **Deciders**: founding team
- **Supersedes**: —
- **Superseded by**: —

## Context

Users author trading strategies in this app. We need a way for users to express a strategy as buy / sell / hold actions per bar (Specs/05).

Two extreme designs both have problems:

- **UI-only rule editor**: easy for casual users, but the moment someone needs an `if/else` not in our ruleset, they're stuck.
- **Code editor only** (Python / Lua / JS): infinite expressivity, but a steep cliff for users who just want "buy when SMA(20) crosses above SMA(50)".

The spec for the strategy module (Specs/05) needs to commit to one approach.

## Decision

Support **both**, with a unified C++ interface (`IStrategy`) underneath.

- **Rule mode** — declarative JSON edited via a Qt form. Covers crossovers, threshold conditions, position sizing, stop-loss/take-profit. The form generates and consumes the JSON directly.
- **Lua mode** — a Lua 5.4 script in a sandbox (sol2 binding, no `os` / `io` / `package`, debug-hook-based cancellation). Receives the same `Context` the rule engine sees, plus arbitrary control flow.

Both compile to the same `IStrategy` C++ interface. The engine doesn't know which mode produced the strategy.

Plugin authors can add a third mode (native C++ `IStrategy` impls) via the plugin SDK (Specs/08).

## Consequences

**Positive:**

- New users start in rule mode and ship something working in minutes.
- Power users who outgrow rules switch to Lua without changing tools or moving to a different runtime.
- The single `IStrategy` interface keeps the engine simple.
- Both modes have their parameters and metadata persisted in plain text, so strategies are diff-able and reviewable in PRs.

**Negative:**

- Two authoring paths to maintain (rule compiler + Lua bridge).
- Risk of users stalling between the two — "I want one feature rules don't cover, do I rewrite in Lua?" The migration from rule JSON to Lua is partially automatic (we generate Lua scaffolding from the rule JSON), but there's still a one-way trip.
- Lua sandbox has to be maintained (escape attempts, new Lua versions).

**Mitigations:**

- Reference strategy (`sma-cross-aapl`) implemented in **both** modes; CI test asserts identical trades on a fixed dataset (Specs/05 §8). This locks in semantic equivalence and protects against drift.
- Sandbox tests cover known escape attempts (loading `os`, opening files, etc.) — Specs/05 §10.
- Documentation in Specs/05 explicitly tells users when to start in Lua (anything beyond crossovers + thresholds).

## Alternatives considered

1. **Lua only**. Rejected: requires every user to read code, even for trivial strategies. The form mode lowers the barrier dramatically without limiting power users.
2. **Rule JSON only**. Rejected: insufficiently expressive. Users will hit walls (custom bar-aggregation, non-trivial stop logic, multi-condition combinations).
3. **Python embedded (CPython)**. Rejected: ~10× the embedding footprint of Lua; GIL complicates cancellation; pip dependency story across three OSes is hard.
4. **JavaScript via QuickJS or Duktape**. Considered. Rejected: comparable footprint to Lua but a more error-prone language for numeric work; fewer trading-domain examples; `==` vs `===` foot-guns.
5. **LuaJIT instead of Lua 5.4**. Considered: faster. Rejected: Apple Silicon support is awkward; we don't need its perf for per-bar decisions; complexity / risk not worth it.
6. **Visual-only block editor (Scratch-like)**. Rejected: massive UI investment for a single-developer team, and locks expressivity to whatever blocks we ship.
7. **Native C++ plugins as the only path**. Rejected: requires every strategy author to compile, ship a binary per OS, and trust users with their build environments. We do support this via the plugin SDK (Specs/08), but it's not the primary authoring path.

## References

- [`../Specs/05StrategyAuthoring.md`](../Specs/05StrategyAuthoring.md)
- [`../Specs/08PluginSystem.md`](../Specs/08PluginSystem.md)
