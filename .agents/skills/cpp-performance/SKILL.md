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

The accepted measurable performance budget is in `Docs/Specs/DataLayer.md`.
The engine specification identifies behavior whose implementation may become a
hot path but does not itself define a numeric budget. Relevant paths are:

- **Engine bar loop** (`Docs/Specs/EngineReplayPnL.md` §2): one canonical event sequence per slice.
- **Indicators `update()`** (`Docs/Specs/Indicators.md`): O(1) amortized; no allocation after warm-up.
- **Replay presentation** and **Data prefetch** become hot paths only when a
  checked-in benchmark or profile identifies them as such.

Code that runs once at startup or rarely is **not** a hot path. Don't micro-optimize it.

## Detection checklist (smells to flag in review)

When reading or writing C++, scan for these. Each one needs justification or a fix.

### Containers and lookups

| Smell | Why it's slow | Fix |
|---|---|---|
| `std::map` for measured hot lookups that do not need ordering | tree traversal and poor locality | consider `std::unordered_map` after measuring and reviewing determinism |
| `std::list` / `std::forward_list` | terrible cache locality | `std::vector`, `std::deque` |
| `std::unordered_map<std::string, V>` with frequent lookup by `string_view` | allocates a temporary `std::string` per lookup unless using transparent hashers | use heterogeneous lookup with `transparent` hasher/equal |
| Linear `std::find` on a sorted container | O(n) when O(log n) is free | `std::ranges::lower_bound` / `binary_search` |
| `vector::push_back` in a tight loop without `reserve()` | repeated reallocation | `reserve(n)` before the loop |
| `vector` of polymorphic objects | cache-unfriendly indirection | `std::variant` if the alternatives are known; otherwise vector of pointers grouped by type |

### Copies and allocations

**Outside hot paths**, redundant or otherwise “unnecessary” copies are
**permitted** when they simplify an API or control flow: pass-by-value
parameters for small trivial values, named temporaries, and returned structs.
Reserve strict copy avoidance for measured hot paths identified under
[`Docs/DefinitionOfDone.md`](../../../Docs/DefinitionOfDone.md), or when the
payload is large.

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
| Recompute an SMA/EMA from scratch every bar | use the rolling indicator API (`Docs/Specs/Indicators.md`) — never recompute |
| `std::sort` followed by `std::unique` | `std::set` if order doesn't matter; or sort+unique only if you need it once |
| `std::accumulate` in a hot loop with default `+` on `double` over a long sequence | use Kahan summation or pairwise summation if precision matters |

### I/O and DB

| Smell | Fix |
|---|---|
| Per-record console or file output in a hot loop | batch structured records and flush outside the loop using an implemented logging seam |
| DuckDB query per bar | release execution streams immutable segments; DuckDB is developer-only (`Docs/Specs/DataLayer.md`) |
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

Accepted cache constraints:

- Cache only when the owning spec and a measured caller define an invalidation and memory policy.
- Planned indicators use bounded display history (`Docs/Specs/Indicators.md` §4).
- Replay checkpoints are persisted engine records, not a license to invent an in-memory cache contract.

If you're proposing a new cache, write down: keys, eviction policy, max size, invalidation trigger, expected hit rate. No "future work" caches.

## Compile-time levers

Use a checked-in optimized preset for measurements. Do not invent compiler
flags, enable LTO, force inlining, or add branch hints without before/after
evidence and verification on the supported toolchain.

## Measurement

No checked-in benchmark framework currently exists. For a performance claim,
use a repository benchmark when one is added or document a reproducible command,
fixture identity, machine/runner class, build preset, repetitions, and raw
before/after samples in the PR. Do not invent a benchmark target or dependency.

Do not call a performance budget merge-blocking until `Docs/Specs/CiDevFlow.md`
and the checked-in workflow both say it is implemented.

## Profile, don't guess

Before "optimizing", profile:

- Linux: use `perf record -g` with an executable discovered in the checkout,
  then inspect it with `perf report`; do not copy an illustrative target name.
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

1. Did you change a documented or measured hot path? → run the verified
   benchmark or provide the reproducible measurement above.
2. Did you add an allocation inside a per-bar or per-slice loop? → justify or remove.
3. Did you add a `std::function` / virtual call inside a tight loop? → replace.
4. Did you introduce a `std::map`? → was `std::unordered_map` considered?
5. Does the canonical determinism test still pass (`Docs/Specs/EngineReplayPnL.md` §9)? Optimizations sometimes change FP order.
