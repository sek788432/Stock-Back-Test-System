---
name: cpp-modern-style
description: >-
  Enforce modern C++20 style in this repository's C++ code: prefer std types,
  RAII, ranges, concepts, structured bindings; ban C-style idioms (raw new/delete,
  C arrays, NULL, sprintf, K&R output params, plain enum, typedef). Use when
  writing or editing any .h / .hpp / .cpp / .cc / .cxx / .ipp file in this repo,
  reviewing C++ code, refactoring legacy code, or when the user mentions modern
  C++, C++20, lowerCamelCase, naming, or style.
---

# Modern C++20 Style

Code in this repo is C++20. Every new function, class, or change must look like 2024+ C++. C-style code is rejected at review.

## Naming (per project rule)

### Identifiers

- Variables, methods, free functions, namespaces → **lowerCamelCase** (`barIndex`, `submitOrder`, `bte::engine`).
- Types (classes, structs, enums, type aliases) → **UpperCamelCase** (`Bar`, `OrderType`).
- Scoped enum values → lowerCamelCase (`OrderSide::buy`).
- Private members → trailing underscore (`cash_`).
- Public POD struct members → plain (`Bar::open`).

### File names

- **New C++ headers and sources:** **UpperCamelCase** stem; align with the primary type or translation unit (`Bar.h`, `Bar.cpp`, `OrderBook.cpp`). Extensions: `.h`, `.cpp`.
- **C++ unit test translation units:** **`UnitTest_<Thing>.cpp`**, where `<Thing>` is UpperCamelCase and matches the type or subsystem under test (`UnitTest_Bar.cpp` ↔ `Bar`).
- **`*.md`:** no project naming convention (docs may use any consistent local style).

### Directory names

- **New repository / module directories** use **UpperCamelCase**, aligned with file naming: e.g. `Src/`, `Docs/`, `Docs/Governance/`, `Tests/`, `Backend/Core/`. CMake’s out-of-source binary tree uses **`Output/`** (see root `CMakePresets.json`; preset dirs like `Output/dev`), not lowercase `build/`.
- Existing tooling folders (e.g. `Cmake/`, `.github/`) follow historical layout; prefer UpperCamelCase for **new** sibling directories.

- Header guard: `#pragma once` always.
- All code lives under namespace `bte::<module>`.

## Banned C-style idioms

| Banned | Modern replacement |
|---|---|
| `new` / `delete`, `malloc` / `free` | `std::make_unique<T>(...)`, `std::make_shared<T>(...)`, value types |
| `T[N]` C arrays | `std::array<T, N>`, `std::vector<T>` |
| `T*` for owning ptr | `std::unique_ptr<T>`; for non-owning use `T*` (raw, never owning) or `T&` |
| `NULL` / `0` for ptr | `nullptr` |
| `typedef` | `using` |
| `enum X { ... }` | `enum class X { ... }` (always scoped) |
| `sprintf` / `printf` / `fprintf` | `std::format`, `fmt::format`, `spdlog` |
| `strcpy` / `strcat` / `strlen` on strings | `std::string`, `std::string_view` |
| `const char*` parameter | `std::string_view` (read-only) or `const std::string&` (must own) |
| Output params (`f(int& out)`) | Return value (`int f()`), `std::pair`, `std::tuple`, struct, or `std::optional` |
| `goto` | structured control flow |
| `(int)x` C cast | `static_cast<int>(x)`, `reinterpret_cast`, `const_cast` (last two are red flags) |
| Macro constants `#define PI 3.14` | `inline constexpr double pi = 3.14;` |
| Macro functions `#define MAX(a,b)` | `constexpr` or `consteval` function template |
| Header `extern int x;` globals | `inline` variables in headers, or function-local statics |
| Hand-rolled `for (int i = 0; i < n; ++i)` over containers | range-for, `std::ranges::*` |

## Required modern features

Use these by default; reach for them before reaching for older equivalents.

### Types

- `std::span<T>` for non-owning views into contiguous memory.
- `std::string_view` for non-owning string views.
- `std::optional<T>` for "maybe a value" (no in-band sentinels like `-1`).
- `std::variant<A, B>` for sum types (no tagged unions, no inheritance hierarchies for two-state values).
- `std::expected<T, E>` if available; otherwise this repo's `bte::core::Result<T, Error>` (see Docs/Specs/03).
- `std::chrono` for all time. Never use `time_t` or seconds-as-int.
- `std::filesystem::path` for paths. Never `const char*` or `std::string` for filesystem names.

### Initialization

```cpp
Bar bar { .ts = ts, .open = 100.0, .close = 101.0, .high = 102.0, .low = 99.0 };  // designated init
auto [ts, open, high, low, close, volume] = decompose(bar);                       // structured bindings
auto values = std::vector{1, 2, 3};                                               // CTAD
constexpr auto barsPerHour = 60;                                                  // constexpr by default
```

### Algorithms

- Prefer `std::ranges::*` over the iterator-pair overloads.
- `std::ranges::sort(v)`, `std::ranges::find(v, x)`, `std::ranges::transform`.
- Use views (`std::views::filter`, `std::views::transform`, `std::views::take`) for chained operations.
- `std::span` for "I take a contiguous range, I don't own it".

### Functions / templates

- `auto` return type for short helpers; explicit return type for public APIs.
- Use **concepts** to constrain templates (`requires` clauses or `template <std::integral T>`).
- Prefer free functions in a namespace over static members.
- Default function arguments are fine; default template arguments are fine.
- `[[nodiscard]]` on every function whose return value is a value the caller must check (most factories, all `Result<T>` returns).

### Constexpr / consteval

Compute at compile time when possible:

```cpp
constexpr int barsPerYear(std::string_view schemaName) noexcept;
consteval auto buildLookupTable();
```

### Format and logging

- `std::format("{} bars from {}", n, sym)` — never `sprintf`.
- `spdlog::info("...")` from the relevant `bte::core::log::*` logger.
- Never log raw `errno` — wrap into `bte::core::Error`.

## Pointer policy

| Use case | What to write |
|---|---|
| Sole owner | `std::unique_ptr<T>` |
| Shared ownership (rare; justify) | `std::shared_ptr<T>` |
| Optional borrow | `T*` (raw, non-owning) — document with comment |
| Required borrow | `T&` |
| View into owned data | `std::span<T>`, `std::string_view` |
| Polymorphic factory return | `std::unique_ptr<IInterface>` |

Never use `auto_ptr` (gone), never `boost::shared_ptr` (we have std), never raw `new` outside placement-new for special allocators.

## Headers and includes

- `#pragma once` always; no include guards.
- Include order in every `.cpp`:
  1. The matching header (`Foo.cpp` includes `Foo.h` first).
  2. C++ standard headers (alphabetical).
  3. Third-party headers (alphabetical).
  4. Project headers (alphabetical).
- No `using namespace std;` ever, in any scope. Use it for short typedef-like aliases inside functions only when needed.
- Public headers go in `Src/Backend/<Module>/Include/Bte/<Module>/`. Private headers stay under `Src/`.

## Error handling

This repo uses **no exceptions across module boundaries** (Docs/Specs/01 §4). Public APIs return `bte::core::Result<T, Error>`. Internal helpers may throw; the boundary catches and wraps.

```cpp
[[nodiscard]] core::Result<int64_t> rowCount(...) const;     // good
int64_t rowCount(...) const;  // bad: hides failure
int64_t rowCount(..., bool* ok); // bad: out param, C-style
```

Never use error codes returned by reference. Never use `errno`. Never throw across `extern "C"` plugin boundaries.

## Comments

- Comments explain **why**, not **what**. The code says what.
- Use `//` line comments. `/* */` only for license headers.
- Doxygen `///` only on public APIs in headers, when behavior isn't obvious from signature.

## What to do when you see legacy code

If you must edit a function that violates these rules:

1. Fix the violation in your touched lines (cheap).
2. If broader cleanup is needed, leave a `// TODO(bte-modernize): ...` comment and open an issue.
3. Do not silently leave new code in legacy style "to match surroundings".

## Verification

Before completing any C++ change:

1. Run `clang-format -i` on all touched files (project's `.clang-format` is authoritative).
2. Run `clang-tidy` (uses repo's `.clang-tidy`). Zero new warnings on touched files.
3. Confirm no banned idioms above appear in your diff.
4. Confirm naming matches rules above.

If `cpp-static-analysis` skill is also active, follow its tooling instructions. If not, see `Docs/Specs/10_CI_Dev_Flow.md` §3.
