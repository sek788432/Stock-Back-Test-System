---
name: cpp-thread-safety
description: >-
  Make C++ code in this repository thread-safe and leak-free by default: enforce
  RAII for every resource, prefer immutable cross-thread snapshots over shared
  mutable state, use std::unique_ptr / std::jthread / std::scoped_lock / std::stop_token
  / std::atomic correctly, and run with sanitizers in dev. Use when writing or
  reviewing any code that touches threads, mutexes, atomics, raw pointers,
  manual new/delete, file handles, sockets, callbacks across thread boundaries,
  Qt signals across threads, or when the user mentions race, deadlock, leak,
  thread, async, sanitizer, or memory.
---

# Thread Safety and Memory Hygiene

Two rules drive everything:

1. **Every resource is owned by a stack-allocated handle (RAII).** No raw `new`/`delete`. No raw `mutex.lock()/unlock()`.
2. **Don't share mutable state across threads.** Pass owned values or immutable snapshots.

If you need to share mutable state, you need a written reason and a defined synchronization protocol.

## Ownership: RAII or it's wrong

Every resource has exactly one owner whose destructor releases it. No exceptions.

| Resource | Owner type |
|---|---|
| Heap memory (sole owner) | `std::unique_ptr<T>` |
| Heap memory (shared, justify in PR) | `std::shared_ptr<T>` |
| Mutex hold | `std::scoped_lock` (multi-mutex safe) or `std::lock_guard` |
| Reader-writer mutex hold | `std::shared_lock` (read), `std::unique_lock` (write) |
| Thread | `std::jthread` (auto-joins, stop-token aware) |
| File | `std::ofstream` / `std::ifstream` / `std::fstream` |
| OS handle (socket, fd, dlopen) | RAII wrapper class with deleted copy + move-only semantics |
| Lua state, DuckDB connection, etc. | Wrapper class; declare destructor; rule of five |
| `dlopen` handle | Wrapper that calls `dlclose` in destructor |

Banned: bare `new`/`delete`, bare `mutex.lock()`, `pthread_create`, `std::thread::detach()` (use `jthread` + `stop_token`), `std::auto_ptr` (gone in C++17).

## The "rule of zero" first, "rule of five" only when forced

Most classes in this repo should be **rule of zero**: members are RAII types, the compiler synthesizes correct copy/move/destructor.

```cpp
class Portfolio {                 // rule of zero — perfect
public:
    explicit Portfolio(double cash);
    // no copy/move/destructor; all members manage themselves

private:
    double cash_ = 0.0;
    std::unordered_map<std::string, Position> positions_;
};
```

Define copy/move/destructor only when wrapping a non-RAII resource (`dlopen`, `lua_State*`, raw OS handle). Then implement **all five** (or `=default` / `=delete` each) — never let the compiler generate one and you write another.

## Cross-thread communication

### Default: pass by value or immutable snapshot

The engine, data layer, and Qt UI run on different threads (Docs/Specs/01 §3). They communicate via:

```cpp
// from Engine thread → UI thread
emit barProcessed(BarSnapshot{ /* trivially copyable */ });   // QueuedConnection deep-copies
```

`BarSnapshot`, `TradeSnapshot`, `PortfolioSnapshot` are trivially copyable values declared in `Bte/Core/Snapshots.h`. Never ship a `shared_ptr<Bar>` across threads to avoid a copy — the snapshot copy is the safety boundary.

### When you must share

If you genuinely need shared mutable state, the rules are:

1. **Document the protocol** in a comment above the shared field: who locks what, in what order.
2. Use `std::scoped_lock` for any combination of mutexes (deadlock-free locking).
3. Keep critical sections **as short as possible**. No I/O, no logging, no callbacks held under a lock.
4. Use `std::shared_mutex` if reads dominate and writes are rare. Otherwise `std::mutex`.
5. For one-time init, use `std::call_once` + `std::once_flag`, not double-checked locking.

```cpp
class IndicatorRegistry {
public:
    void registerKind(std::string kind, Factory f) {
        std::scoped_lock lock(mutex_);              // RAII
        factories_.insert_or_assign(std::move(kind), std::move(f));
    }

    Factory lookup(std::string_view kind) const {
        std::shared_lock lock(mutex_);
        auto it = factories_.find(std::string{kind});
        return it == factories_.end() ? Factory{} : it->second;
    }

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Factory> factories_;
};
```

### Atomics

Use `std::atomic<T>` for primitive shared state (counters, flags). Never roll your own with `volatile` — `volatile` is **not** a synchronization primitive in C++.

```cpp
std::atomic<bool> paused_ {false};       // OK
volatile bool paused_ = false;           // WRONG — does not synchronize
```

Memory order: default to `std::memory_order_seq_cst` (the implicit default). Use weaker orderings only with a written justification (typical: `release`/`acquire` on a flag paired with data).

### Cancellation

Long-running work uses `std::stop_token` (paired with `std::jthread` or `std::stop_source`):

```cpp
core::Result<void> runBacktest(std::stop_token stop) {
    while (auto bar = stream.next()) {
        if (stop.stop_requested()) {
            return Error{ErrorCode::cancelled, "user cancelled"};
        }
        // ...
    }
    return {};
}
```

Lua scripts get cancellation via a debug hook checking `stop_token` (Docs/Specs/05 §4.2).

### Qt-specific rules

- The Qt main thread is the **only** thread allowed to touch widgets. Always.
- Worker → UI: `emit signal(...)` connected with `Qt::QueuedConnection`. The signal arguments must be `Q_DECLARE_METATYPE`'d.
- UI → worker: `QMetaObject::invokeMethod(worker, ..., Qt::QueuedConnection)`.
- A `QObject` belongs to the thread that created it (or that you `moveToThread`'d it to). All slots run on that thread.
- Never call `widget->update()` or any `QWidget` method from a worker thread.

## Common race patterns to refuse

When you see these in code, fix them:

```cpp
// 1. Shared raw pointer with no synchronization
class Engine {
    Strategy* strategy_;     // who owns? who reads?
};
// → make it std::unique_ptr<IStrategy>, owned by exactly one thread.

// 2. Read-modify-write on a non-atomic
counter_++;                    // race
// → std::atomic<int> counter_; counter_.fetch_add(1);

// 3. Iterator invalidation
for (const auto& kv : map_) {
    if (cond) map_.erase(kv.first);     // boom, unless single-threaded and clear
}
// → std::erase_if(map_, [](auto& kv){ return cond(kv); });

// 4. Lock acquired in different orders → deadlock
std::scoped_lock lock(a_, b_);          // safe
std::lock_guard la(a_); std::lock_guard lb(b_);  // unsafe if other code does b then a
// → always use std::scoped_lock for >1 mutex.

// 5. Long-lived lock around I/O
std::lock_guard lock(mutex_);
saveToDisk(state_);             // disk while holding mutex blocks everyone
// → snapshot under lock, save outside the lock.

// 6. Returning reference to mutex-protected data
const State& state() const {
    std::scoped_lock lock(mutex_);
    return state_;              // reference outlives the lock — caller races
}
// → return State (by value) under the lock.
```

## Memory leak prevention

The repo's CI runs **AddressSanitizer + LeakSanitizer + UBSan** on the `dev` preset (Docs/Specs/09 §1.2). The sanitizers will catch leaks at test exit. To pass:

1. Every owning resource is RAII (above).
2. Cycles in `std::shared_ptr` are forbidden. Use `std::weak_ptr` for back-references.
3. `setParent(...)` in Qt is fine for owning widgets — Qt's parent-child is RAII.
4. `dlopen` handles closed in plugin manager destructor.
5. Self-referential lambdas captured by `[this]` must outlive `this` or be invalidated before `this` dies. Use `QPointer` for Qt objects, `std::weak_ptr` otherwise.

## Sanitizer dev workflow

When iterating locally on threading-sensitive code:

```bash
cmake --preset dev          # has ASan + UBSan + LeakSan + debug Qt
cmake --build --preset dev
ctest --preset dev          # sanitizers fire on any violation, test fails

# For races specifically (ASan and TSan can't combine):
cmake --preset dev-tsan     # adds -fsanitize=thread, drops ASan
ctest --preset dev-tsan
```

If a sanitizer fires, **do not suppress** the report. The bug is real until proven otherwise. Suppressions in `Tests/sanitizer-suppressions.txt` require CODEOWNER approval and an `until:` date.

## Verification before committing

1. Did you introduce any raw `new`, `delete`, `malloc`, `free`? → replace with smart pointer / value.
2. Did you call `mutex.lock()` / `mutex.unlock()` directly? → use `std::scoped_lock`.
3. Did you use `std::thread`? → use `std::jthread` (or justify).
4. Did you cross a thread boundary with raw pointers or non-trivial types not registered as Qt metatype? → use a snapshot value type.
5. Did you add a `volatile` for synchronization? → use `std::atomic`.
6. Did `ctest --preset dev` pass with no sanitizer reports?

If yes to all, the change is good. Otherwise, fix before committing.
