---
name: cpp-oop-design
description: >-
  Enforce clear abstraction and good OOP for C++ code in this repository:
  reject one-shot helpers, prefer composition over inheritance, separate
  interface from implementation, apply the right design pattern (Strategy,
  Factory, Observer, Adapter, Builder, Pimpl, Visitor, Chain, Command, Decorator)
  when the situation calls for it, and keep modules behind narrow public headers.
  Use when designing a new module, adding a new class or interface, refactoring
  duplicated logic, reviewing PR design choices, or when the user mentions
  abstraction, design pattern, refactor, OOP, interface, SOLID, or DRY.
---

# OOP and Design Patterns

Code in this repo must be **easy to maintain six months from now**. That means:

1. Every type has a clear single responsibility.
2. Public surfaces are narrow; implementations are hidden.
3. Duplicated logic is extracted, named, and reused.
4. Patterns are used **when they fit**, not for their own sake.

## SOLID, in plain language for this repo

| Principle | What it means here |
|---|---|
| **S**ingle responsibility | A class does one job. `Portfolio` tracks positions; it does not parse bars. `BarStream` reads bars; it does not compute indicators. |
| **O**pen / closed | Add a real second implementation behind a narrow registry or Strategy seam without touching `Engine` (`Docs/Specs/Architecture.md`, `Docs/Specs/Indicators.md`, and `Docs/Specs/StrategyAuthoring.md`). |
| **L**iskov substitution | Every implementation honors the complete documented contract of its seam; callers need no subtype-specific exception. |
| **I**nterface segregation | Keep a seam focused on one caller need instead of accumulating unrelated methods. |
| **D**ependency inversion | High-level policy depends on a narrow owning-module seam; presentation does not depend on engine internals. |

## Composition over inheritance

Default to **composition**. Inherit only to expose polymorphism.

The snippets in this skill are illustrative C++20, not declarations of
repository APIs. Before using a name in production code, find the owning spec
and implemented header or introduce the type through the normal design review.

```cpp
class ConditionEvaluator final {
public:
    [[nodiscard]] bool evaluate(const Input& input) const;
};

class SelectablePolicy final {
public:
    explicit SelectablePolicy(ConditionEvaluator evaluator)
        : evaluator_{std::move(evaluator)} {}

    [[nodiscard]] bool accepts(const Input& input) const {
        return evaluator_.evaluate(input);
    }

private:
    ConditionEvaluator evaluator_;
};

// Do not create BasePolicyWithEvaluator solely to share evaluator helpers.
```

When you find yourself adding a 3rd level of inheritance, stop. Refactor to composition.

## Reject one-shot code

> "I'll just write this here, it's only used once."

If the same shape (parameters, behavior, validation) appears more than once in a PR, factor it. If a function has a comment like "TODO: generalize later", do it now.

Markers that something should be its own type or free function:

- Two call sites do the same 5+ lines of pre/post work.
- A struct is built up by 4+ lines that always run together → constructor or builder.
- A `std::string` parameter validated to a regex in two places → wrap in a `ValidatedSymbol` value type.
- Two tests share 10+ lines of setup → fixture.

## Interface vs implementation separation

For every C++ module:

```
Src/Backend/<Module>/
├── Include/Bte/<Module>/         # PUBLIC — what others see
│   └── <PublicApi>.h             # interfaces, value types, factories
└── Private/                       # PRIVATE — .cpp + internal headers
    ├── <Internal>.h
    └── <PublicApi>.cpp
```

Public headers contain:

- Pure abstract interfaces (`IFoo`).
- Value types (`Bar`, `Order`).
- Factory functions (`createFoo(args) -> Result<unique_ptr<IFoo>>`).
- Concept declarations.

They **do not** contain:

- Implementation classes (those go in private headers).
- Implementation includes (don't drag third-party headers into public headers if you can avoid it — use forward declarations and Pimpl).
- Anything that changes when implementation changes.

This keeps build times low and module seams stable (`Docs/Specs/Architecture.md`).

## Design patterns we use in this repo

Apply these when the shape matches. Don't reach for them just to use the name.

### Strategy

A behavior selected at runtime, with a uniform interface.

The accepted Strategy seam serves Selectable Conditions and Python worker
commands. Its exact public C++ types must come from implemented headers and
[`Docs/Specs/StrategyAuthoring.md`](../../../Docs/Specs/StrategyAuthoring.md),
not from examples in this skill.

When to apply: you have an algorithm with multiple variants, callers choose at runtime, all variants share a small interface.

### Factory

Construction logic lives in one place; callers don't know concrete types.

```cpp
enum class SmoothingKind { simple, exponential };

class Smoother {
public:
    virtual ~Smoother() = default;
    [[nodiscard]] virtual double update(double value) = 0;
};

[[nodiscard]] std::unique_ptr<Smoother>
makeSmoother(SmoothingKind kind);
```

When to apply: configuration selects among at least two implemented concrete types.

### Observer (Qt signals/slots)

Decoupled notification.

```cpp
class ProgressEmitter final : public QObject {
    Q_OBJECT

signals:
    void progressChanged(ProgressSnapshot snapshot);
};
```

For cross-thread delivery, register the snapshot metatype and use a queued
connection as required by `cpp-thread-safety`; the observer pattern does not
make direct widget access from a worker safe.

When to apply: backend produces events; multiple UI views consume them; producer doesn't know about consumers.

### Adapter

Wrap an external type to fit your interface.

- The implemented Qt Charts view fits the real `IChartView` seam; the accepted
  future project-owned `QPainter` view must fit that same seam
  (`Docs/Specs/FrontendQt.md` §4).

When to apply: a third-party type doesn't match your interface, and you don't want callers to depend on the third party.

### Builder

Step-by-step construction without a giant constructor.

When to apply: an object has many optional fields and validation rules; you want fluent or staged construction.

### Pimpl (Pointer to Implementation)

Hide implementation behind a `unique_ptr<Impl>` member.

```cpp
// Foo.h (public)
class Foo {
public:
    Foo();
    ~Foo();
    Foo(Foo&&) noexcept;
    Foo& operator=(Foo&&) noexcept;
    // ... methods

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
```

When to apply: you need faster rebuilds, stable public layout, or to hide a heavy include.

Cost: extra heap allocation. Don't pimpl tiny value types.

### Visitor (with `std::variant`)

Operate on a closed set of types.

```cpp
using RuleNode = std::variant<CompareNode, CrossesNode, AndNode, OrNode>;

double eval(const RuleNode& n, const Context& ctx) {
    return std::visit([&](auto&& node) { return evalImpl(node, ctx); }, n);
}
```

When to apply: a sealed family of types (rule AST nodes, message types). Cleaner than virtual + dynamic_cast.

### Chain of responsibility / Pipeline

A series of stages, each transforming or short-circuiting.

- The engine bar loop: `data → indicators → strategy → broker → portfolio → metrics` (`Docs/Specs/EngineReplayPnL.md`).
- Order validation: size checks → cash checks → leverage checks → risk caps.

When to apply: linear processing where each step has a clear local responsibility.

### Command

Encapsulate a request as an immutable value when it must be deferred, queued,
or audited. Use the actual owning-module order and strategy-command types found
in the checkout; this skill does not declare them.

### Decorator

Wrap a type to add behavior, preserving the interface.

```cpp
class Reader {
public:
    virtual ~Reader() = default;
    [[nodiscard]] virtual std::optional<Record> next() = 0;
};

class CountingReader final : public Reader {
public:
    explicit CountingReader(Reader& inner) noexcept : inner_{inner} {}

    [[nodiscard]] std::optional<Record> next() override {
        auto record = inner_.next();
        if (record.has_value()) {
            ++count_;
        }
        return record;
    }

private:
    Reader& inner_;
    std::size_t count_ = 0;
};
```

The required reference makes null impossible; the wrapped reader must outlive
the decorator. Use owned composition instead when that lifetime is not already
guaranteed by the caller.

When to apply: cross-cutting concerns (logging, metrics, retries) without modifying the wrapped class.

### Templated policy classes (compile-time strategy)

When the variant is known at compile time and is in a hot path, prefer policy classes over virtual.

```cpp
template <typename FillPolicy>
class Simulator final {
public:
    explicit Simulator(FillPolicy fillPolicy)
        : fillPolicy_{std::move(fillPolicy)} {}

    [[nodiscard]] Fill apply(const Order& order, const Quote& quote) const {
        return fillPolicy_(order, quote);
    }

private:
    FillPolicy fillPolicy_;
};
```

When to apply: hot path, fixed at compile time, you can afford the binary size.

## Anti-patterns to refuse

- **God classes** (`Engine` doing everything from data to UI). Split.
- **Anemic data classes** + service classes everywhere → some methods belong on the data class.
- **Inheritance for code reuse** ("base class with shared helpers"). Use composition, free functions, or mixins (CRTP).
- **`switch` on a type tag for behavior**. Use polymorphism (virtual or `std::variant` + `std::visit`).
- **Boolean parameters that change behavior** (`f(x, true)`). Two functions or an enum.
- **Out-parameters**. Return a value or a struct (`Docs/Specs/BackendCore.md`).
- **Singleton Manager classes** for state. Prefer explicit construction and
  dependency injection. Any stateless process-wide object still needs a real
  implemented caller and tests.
- **Interfaces with one impl, never likely to grow**. Don't pre-abstract; introduce the interface when the second impl arrives.
- **Header-only "convenience" libraries** that drag in megabytes of templates everywhere. Hide impl in `.cpp`.

## When to introduce an abstraction

Two-strikes rule. The first time a shape appears, write it concrete. The second time, factor it into a function or class. The third time, you're already in trouble.

If you're tempted to introduce an interface for "future flexibility", don't. **YAGNI** wins. Add the abstraction when the second concrete impl appears, not before.

## Module boundaries (this repo)

Use the current graph in `Docs/Specs/Architecture.md` as authority. In
particular, backend modules stay Qt-independent, Bindings translates backend
values, and Frontend does not reach into engine internals. Search current CMake
targets and public headers before naming an edge; planned target modules are not
proof that their implementations already exist.

If you find yourself adding a new include that violates this, it's a design smell. Either move the type to a lower module or introduce a new interface in a lower module that the higher module implements.

## Verification before committing

1. Does any new file have a single, clear, name-able responsibility? If you can't explain it in a sentence, split it.
2. Did you copy 5+ lines of logic from another file? → factor.
3. Does any new public header pull in a heavy third-party type? → Pimpl or forward declaration.
4. Did you add a virtual to a hot-path interface for a single concrete impl? → drop the virtual until the second impl arrives.
5. Does the change respect the module dependency graph (`Docs/Specs/Architecture.md` §2)?
6. Did you reach for inheritance when composition fits? Reconsider.
