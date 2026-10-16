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
| OS handle required by an implemented module | Narrow move-only RAII wrapper with the documented close operation |

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

Define copy/move/destructor only when wrapping a non-RAII resource required by
an implemented module. Then explicitly default or delete the applicable special
members and document the ownership contract.

Illustrative wrapper—the names below are not repository APIs:

The platform-specific `closeHandle` operation in this pattern must be
non-throwing; a destructor or move assignment must not leak a close failure as
an exception.

```cpp
class NativeHandle final {
public:
    explicit NativeHandle(Handle value) noexcept : value_{value} {}
    ~NativeHandle() { reset(); }

    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;

    NativeHandle(NativeHandle&& other) noexcept
        : value_{std::exchange(other.value_, invalidHandle)} {}

    NativeHandle& operator=(NativeHandle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, invalidHandle);
        }
        return *this;
    }

private:
    void reset() noexcept {
        if (value_ != invalidHandle) {
            closeHandle(value_);
            value_ = invalidHandle;
        }
    }

    Handle value_ = invalidHandle;
};
```

## Cross-thread communication

### Default: pass by value or immutable snapshot

The accepted engine/data/UI model is defined in
`Docs/Specs/Architecture.md` §4. Verify the checked-in worker and Qt adapter
types before naming them. Cross-thread delivery uses owned values or immutable
snapshots through queued delivery; no skill example declares a public snapshot
header or signal that the repository does not contain.

### When you must share

If you genuinely need shared mutable state, the rules are:

1. **Document the protocol** in a comment above the shared field: who locks what, in what order.
2. Use `std::scoped_lock` for any combination of mutexes (deadlock-free locking).
3. Keep critical sections **as short as possible**. No I/O, no logging, no callbacks held under a lock.
4. Use `std::shared_mutex` if reads dominate and writes are rare. Otherwise `std::mutex`.
5. For one-time init, use `std::call_once` + `std::once_flag`, not double-checked locking.

Illustrative synchronized value—the type names are deliberately generic:

```cpp
class SnapshotCache final {
public:
    void replace(Snapshot snapshot) {
        std::scoped_lock lock{mutex_};
        snapshot_ = std::move(snapshot);
    }

    [[nodiscard]] Snapshot read() const {
        std::scoped_lock lock{mutex_};
        return snapshot_;
    }

private:
    // Synchronization protocol: every access to snapshot_ holds mutex_; this
    // class acquires no other lock and invokes no callback while it is held.
    mutable std::mutex mutex_;
    Snapshot snapshot_;
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

Check cancellation at bounded intervals and translate it through the actual
owning module's `bte::core::Result<T>` error contract.

```cpp
enum class ConsumeStatus { completed, cancelled };

template <typename Next, typename Consume>
[[nodiscard]] ConsumeStatus consumeUntilStopped(std::stop_token stopToken,
                                                Next next,
                                                Consume consume) {
    while (!stopToken.stop_requested()) {
        auto value = next(stopToken);
        if (!value.has_value()) {
            return stopToken.stop_requested() ? ConsumeStatus::cancelled
                                              : ConsumeStatus::completed;
        }
        consume(*value, stopToken);
    }
    return ConsumeStatus::cancelled;
}
```

This example shows cancellation placement and an explicit outcome. Both
callbacks must cooperate with the supplied token and return within the
owning contract's bounded interval. Production code must translate the outcome
into the owning module's documented `bte::core::Result<T>` cancellation error.

Python worker hooks use the deadline and cooperative-then-forced cancellation
contract in `Docs/Specs/StrategyAuthoring.md` §4.

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
class WorkerState {
    State* state_;     // who owns it, and which thread may read it?
};
// → prefer a value or unique owner confined to one thread.

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

The repo's sanitizer CI matrix runs **AddressSanitizer + LeakSanitizer +
UBSan** with `dev-sanitize`, and TSan separately with `dev-tsan`. The sanitizers
catch leaks, undefined behavior, and data races during registered tests. To pass:

1. Every owning resource is RAII (above).
2. Cycles in `std::shared_ptr` are forbidden. Use `std::weak_ptr` for back-references.
3. `setParent(...)` in Qt is fine for owning widgets — Qt's parent-child is RAII.
4. Self-referential lambdas captured by `[this]` must outlive `this` or be
   invalidated before destruction. Use `QPointer` for Qt objects and a justified
   `std::weak_ptr` only when shared ownership is real.

## Sanitizer dev workflow

When iterating locally on threading-sensitive code:

```bash
cmake --preset dev-sanitize
cmake --build --preset dev-sanitize --parallel
ctest --preset dev-sanitize --no-tests=error

# For races specifically (ASan and TSan can't combine):
cmake --preset dev-tsan
cmake --build --preset dev-tsan --parallel
ctest --preset dev-tsan --no-tests=error
```

If a sanitizer fires, **do not suppress** the report. No sanitizer suppression
file is wired into the repository; adding one requires a focused tooling change
and maintainer approval.

## Verification before committing

1. Did you introduce any raw `new`, `delete`, `malloc`, `free`? → replace with smart pointer / value.
2. Did you call `mutex.lock()` / `mutex.unlock()` directly? → use `std::scoped_lock`.
3. Did you use `std::thread`? → use `std::jthread` (or justify).
4. Did you cross a thread boundary with raw pointers or non-trivial types not registered as Qt metatype? → use a snapshot value type.
5. Did you add a `volatile` for synchronization? → use `std::atomic`.
6. Did both applicable sanitizer presets pass with no reports?

Resolve every applicable question before committing; a "yes" to items 1–5 is
a defect, while item 6 requires passing evidence.
