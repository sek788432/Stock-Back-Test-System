---
name: cpp-performance
description: >-
  Detect and fix performance smells in C++ code in this repository: O(n^2) loops,
  unnecessary copies and allocations (outside hot paths, redundant copies of small
  trivial types are permitted for clarity), std::map vs std::unordered_map, virtual
  calls and std::function in hot paths, indicator/data layer recomputation,
  expensive synchronization, and string concatenation inside loops. Suggest the
  right algorithm, container, view, or cache. Use when reviewing or writing code
  in tight loops (engine bar-loop, indicators, replay, data prefetch), when
  benchmarks regress, or when the user mentions slow, hot path, optimize,
  benchmark, profile, allocation, or cache.
---

# C++ Performance

Performance work is **measure → fix → re-measure**, not "I think this is faster". Every claim has a number behind it.

This repo has performance budgets in Docs/Specs/07 §9. Hot paths are:

- **Engine bar loop** (Docs/Specs/07 §4): `data → indicators → strategy → broker → portfolio` per bar.
- **Indicators `update()`** (Docs/Specs/06): O(1) amortized; no allocation after warm-up.
- **Replay tick**: 1ms-class to keep ≥ 60 bars/sec at max speed.
- **Data prefetch**: must outpace the consumer.

Code that runs once at startup or rarely is **not** a hot path. Don't micro-optimize it.

## Detection checklist (smells to flag in review)

When reading or writing C++, scan for these. Each one needs justification or a fix.

### Containers and lookups

| Smell | Why it's slow | Fix |
|---|---|---|
| `std::map` for hot lookups | tree, ~3× slower than hash for typical sizes | `std::unordered_map` (or `boost::unordered_flat_map`, `absl::flat_hash_map` if vendored) |
| `std::list` / `std::forward_list` | terrible cache locality | `std::vector`, `std::deque` |
| `std::unordered_map<std::string, V>` with frequent lookup by `string_view` | allocates a temporary `std::string` per lookup unless using transparent hashers | use heterogeneous lookup with `transparent` hasher/equal |
| Linear `std::find` on a sorted container | O(n) when O(log n) is free | `std::ranges::lower_bound` / `binary_search` |
| `vector::push_back` in a tight loop without `reserve()` | repeated reallocation | `reserve(n)` before the loop |
| `vector` of polymorphic objects | cache-unfriendly indirection | `std::variant` if the alternatives are known; otherwise vector of pointers grouped by type |

### Copies and allocations

**Outside hot paths**, redundant or otherwise “unnecessary” copies are **permitted** when they simplify an API or control flow: pass-by-value parameters for small trivial types (`Bar`, IDs, scalars), extra named temporaries for readability, returning structs by value, etc. Reserve strict copy avoidance for the bar loop, `IIndicator::update()`, replay ticks, data prefetch, and other paths treated as hot in **Docs/DEFINITION_OF_DONE.md** — or when the payload is large (strings, containers).

| Smell | Fix |
|---|---|
| `void f(std::string s)` taking by value when caller has a long string | `std::string_view` if read-only, `const std::string&` if must own; or sink-copy by value when you'll move it into a member |
| `void f(std::vector<T> v)` for read-only | `std::span<const T>` |
| `auto v = computeVector();` then iterating once | range / view chain; avoid materializing |
| `std::string` concat in a loop with `+` | `std::string out; out.reserve(n); for (...) out.append(...);` or `std::format_to(std::back_inserter(out), ...)` |
| `return std::move(x)` of a local | RVO already happens; `std::move` here disables RVO |
| `std::shared_ptr` where `std::unique_ptr` works | avoid the atomic refcount |
| Creating `std::function<...>` in a hot loop | use a templated callable / concrete type / `function_ref` (cheap view) |
| Capturing by value in a lambda when reference is fine and lifetime is OK | `[&]` for local-scope lambdas |
| Defining lambdas inside a hot loop | lift them outside the loop |

### Virtual / indirect calls

`virtual` per bar is fine; per tick of an inner loop is not.

| Smell | Fix |
|---|---|
| Virtual call inside indicator `update()` ring-buffer push | inline the buffer logic; use CRTP if the type is known at compile time |
| `std::function` in the engine's per-bar callback | switch to a concrete callable or compile-time policy |
| `dynamic_cast` in hot path | redesign with `std::variant` + `std::visit` |

### Algorithms

| Smell | Fix |
|---|---|
| Two nested loops where one is hash-able | hashmap → O(n) |
| Re-sort the same data each bar | sort once, then update incrementally |
| Recompute an SMA/EMA from scratch every bar | use the rolling indicator API (Docs/Specs/06) — never recompute |
| `std::sort` followed by `std::unique` | `std::set` if order doesn't matter; or sort+unique only if you need it once |
| `std::accumulate` in a hot loop with default `+` on `double` over a long sequence | use Kahan summation or pairwise summation if precision matters |

### I/O and DB

| Smell | Fix |
|---|---|
| `std::cout` / `printf` in hot loop | `spdlog` async sink, or batch and flush |
| DuckDB query per bar | one query, stream the result (Docs/Specs/04 §3.2) |
| File open/close per bar | hold the handle for the lifetime of the run |
| `std::ifstream` line-by-line with operator>> | `std::getline` + parse, or memory-map for huge files |
| JSON parsed once per bar | parse once at strategy init |

### Strings

| Smell | Fix |
|---|---|
| `std::string` member where `std::string_view` view suffices | view (only if lifetime is clear) |
| Implicit `std::string` from `const char*` in lookups | `std::string_view` parameters + transparent hasher |
| `to_string` of an int in a hot loop | `std::format_to(buf, "{}", n)` into pre-allocated buffer |

### Synchronization

| Smell | Fix |
|---|---|
| `std::mutex` where reads dominate | `std::shared_mutex` |
| Lock around computation that doesn't touch shared state | snapshot under lock; release; compute outside |
| Atomic counter incremented in a tight loop by every thread | per-thread counter + final reduce |
| `condition_variable.wait` without a predicate | always pass a predicate to handle spurious wakeups |

### Memory layout

| Smell | Fix |
|---|---|
| Struct with members in random order, `sizeof` larger than needed | reorder for alignment (largest first) |
| Cold and hot fields in same struct | split — keep hot fields cache-resident |
| Polymorphic AoS for hot loop | SoA (struct-of-arrays) |
| Bool arrays as `std::vector<bool>` proxies | `std::vector<char>` or `std::array<bool, N>` |

## Caching, when it earns its keep

Add a cache only when:

1. The computation is measured to dominate runtime.
2. The cache hit rate is provably > 50% on real workloads.
3. Cache invalidation has a defined trigger (data version bump, time bound, LRU).

Existing repo caches:

- `bteData::DataSource` keeps a 5-second range cache and an LRU bar-chunk cache (Docs/Specs/04 §6).
- Indicators keep `history()` ring buffers (Docs/Specs/06 §6).
- Engine keeps `PortfolioCheckpoint` every N bars (Docs/Specs/07 §5.2).

If you're proposing a new cache, write down: keys, eviction policy, max size, invalidation trigger, expected hit rate. No "future work" caches.

## Compile-time levers

- Build with `-O3 -DNDEBUG` for benchmarks. Never benchmark `Debug` builds — meaningless.
- Enable LTO: `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`.
- Hot helpers: mark `inline` (header) or use `[[gnu::always_inline]]` (sparingly, with measurement).
- `[[likely]]` / `[[unlikely]]` only after profiling shows a branch is mispredicted.
- `constexpr` / `consteval` whenever possible — moves work to compile time.

## Measurement

Repo uses **nanobench** (vendored, single header) in `Tests/Bench/`. For any perf claim:

```cpp
#include <nanobench.h>

ankerl::nanobench::Bench()
    .epochs(50)
    .run("smaUpdate", [&] {
        ankerl::nanobench::doNotOptimizeAway(sma.value());
        sma.update(nextBar());
    });
```

Output a markdown table in the PR description. Compare before/after on the same machine, same build flags, with `cpufreq` set to performance mode if on Linux.

CI smoke perf gate (Docs/Specs/07 §9): a fixture run must complete within budget; regressions fail the run.

## Profile, don't guess

Before "optimizing", profile:

- Linux: `perf record -g ./stockBacktester_bench` then `perf report`.
- macOS: Instruments → Time Profiler → attach to process.
- Windows: WPA / VTune.

If the bottleneck isn't where you thought, your optimization plan changes. Don't waste effort on cold code.

## Anti-patterns: don't do these in this repo

- "Optimizing" without measuring. Doesn't go in.
- Hand-rolled SIMD before exhausting `std::ranges` and compiler vectorization. The compiler is good at autovectorizing aligned `std::span<double>` loops if you write the simple form.
- Lock-free data structures without a coverage-quality test suite + TSan + a written invariants doc. The number of times "lock-free" code is correct on the first try is approximately zero.
- Premature templating "in case we need it polymorphic". Add the polymorphism when you have the second use case.
- Caching at every layer "to be safe". Each cache costs invalidation complexity.

## Verification before committing

1. Did you change anything in a documented hot path? → run the relevant nanobench, paste numbers in PR.
2. Did you add an allocation in `IIndicator::update()` or `Engine::run()` body? → justify or remove.
3. Did you add a `std::function` / virtual call inside a tight loop? → replace.
4. Did you introduce a `std::map`? → was `std::unordered_map` considered?
5. Does the determinism test still pass byte-identical (Docs/Specs/07 §8)? Optimizations sometimes change FP order.
