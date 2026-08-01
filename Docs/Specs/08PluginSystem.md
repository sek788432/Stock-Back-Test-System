# 08 — Plugin System

How third parties extend the **native** surface without recompiling the main application.

**Important:** End-user strategies written in **Lua** or **Python** (`05`, **`11`**) live as files under `<userData>/strategies/` and load through **`bteStrategy`** — they are **not** the DLL plugin ABI in this doc. Same for **natural language → accepted scripts** (`05` §6).

**This document** covers:

1. **Native C++ plugins** — shared libraries (`.so` / `.dylib` / `.dll`) that register strategies, indicators, or fill models.
2. **Runtime wiring** — how native plugins coexist with scripted strategies already described in **`05`**.

---

## 1. What plugins can register

| Surface | Plugin can add | Plugin cannot |
|---|---|---|
| Strategies (C++ classes implementing `IStrategy`) | new `id` types listed in the UI | replace built-in strategies |
| Indicators (classes implementing `IIndicator`) | new `kind`s for the registry | shadow built-in `kind`s |
| Fill models (`IFillModel`) | new entries selectable in `EngineConfig::FillModel::custom` | redefine the four built-ins |
| Data sources (`IDataSource`) | bring custom CSV / Parquet / API feeds | override the DuckDB default |
| UI panels | future — not Phase 1 | — |

Plugins **cannot**:
- Touch UI widgets (the UI thread isn't theirs).
- Bypass the sandbox to read arbitrary files (well, native plugins technically can — see §6 trust model).
- Modify other plugins' state.

---

## 2. Loading model

Plugins live in:

```
<userData>/plugins/
├── myFancyStrategy.so          # Linux
├── myFancyStrategy.dylib       # macOS
└── myFancyStrategy.dll         # Windows
```

On startup the app:

1. Scans the directory.
2. For each file, calls `dlopen` / `LoadLibrary`.
3. Looks up the **single required entry point**: `bteGetPluginManifest`.
4. Validates manifest, then calls `bteRegisterPlugin(...)` to wire registrations.
5. Adds the plugin to the **Plugins tab** with name, version, author, registered items.

A plugin that fails any step is **disabled** with a clear error in the UI; other plugins still load.

---

## 3. The plugin ABI

We expose an **extern "C" boundary** so plugins can be built with any major-compatible compiler / runtime.

### 3.1 Header `bte/plugin.h`

```cpp
#ifndef BTE_PLUGIN_H
#define BTE_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#define BTE_PLUGIN_ABI_MAJOR 1
#define BTE_PLUGIN_ABI_MINOR 0

typedef struct {
    uint32_t abiMajor;        // must equal BTE_PLUGIN_ABI_MAJOR
    uint32_t abiMinor;
    const char* name;         // human-readable
    const char* id;           // reverse-DNS recommended
    const char* version;      // semver
    const char* author;
    const char* description;
} BtePluginManifest;

typedef struct BteRegistrar BteRegistrar;     // opaque

#ifdef __cplusplus
extern "C" {
#endif

// Required: returns static manifest. Called BEFORE register so the host
// can reject incompatible ABIs without running plugin code.
const BtePluginManifest* bteGetPluginManifest(void);

// Required: register strategies/indicators/fill-models/data-sources.
// Returns 0 on success, nonzero error code otherwise.
int bteRegisterPlugin(BteRegistrar* registrar);

// Optional: cleanup before unload.
void bteUnregisterPlugin(void);

#ifdef __cplusplus
}
#endif

#endif
```

### 3.2 The C++ SDK header `bte/plugin.hpp`

For convenience, plugin authors use a thin C++ wrapper that hides the C boundary:

```cpp
#include "bte/plugin.h"
#include "bte/plugin/registrar.hpp"      // C++ wrappers

class MySma200Strategy : public bte::strategy::IStrategy {
    // ...
};

extern "C" const BtePluginManifest* bteGetPluginManifest() {
    static const BtePluginManifest m{
        BTE_PLUGIN_ABI_MAJOR, BTE_PLUGIN_ABI_MINOR,
        "MySma200", "com.example.bte.sma200", "1.0.0",
        "Jane Doe", "200-day SMA reversion strategy"
    };
    return &m;
}

extern "C" int bteRegisterPlugin(BteRegistrar* r) {
    bte::plugin::Registrar reg(r);
    reg.registerStrategy<MySma200Strategy>("com.example.sma200");
    reg.registerIndicator<MyHullMa>("hma");
    return 0;
}
```

Under the hood `bte::plugin::Registrar` calls C functions like `bteRegisterStrategy(r, id, factoryFn)` where `factoryFn` is a stateless C function pointer. This keeps the binary boundary plain C while authors write modern C++.

### 3.3 Compatibility rules

- **Major ABI** bumps only when binary layout / signatures break. A plugin with `manifest.abiMajor != host` is rejected with `ErrorCode::pluginIncompatibleAbi`.
- **Minor ABI** bumps add new optional functions; older plugins continue to work.
- The host compiles with `-fvisibility=hidden`; only the C boundary is exported.

We document this contract in `Src/Backend/Strategy/Include/Bte/Plugin/README.md` so authors don't have to read these specs.

---

## 4. SDK packaging

In each release we ship a `bte-plugin-sdk-<version>-<os>-<arch>.zip` containing:

```
bte-plugin-sdk/
├── include/                          # public headers (Core, Indicators, Strategy)
├── lib/                              # bteCore.{a,lib} import library
├── cmake/BtePluginSdkConfig.cmake    # find_package support
├── examples/
│   └── sma200/                       # sample plugin, builds out-of-the-box
└── README.md
```

Plugin authors then:

```cmake
find_package(BtePluginSdk REQUIRED)
add_library(myStrategy MODULE myStrategy.cpp)
target_link_libraries(myStrategy PRIVATE Bte::PluginSdk)
```

`MODULE` (vs `SHARED`) is the right CMake target type for runtime-loaded plugins.

---

## 5. Registration internals

Inside the host:

```cpp
class PluginManager {
public:
    core::Result<void> loadAll(const std::filesystem::path& dir);
    core::Result<void> loadOne(const std::filesystem::path& file);
    void unloadAll();

    std::span<const LoadedPlugin> loaded() const;
};

struct LoadedPlugin {
    std::filesystem::path file;
    BtePluginManifest manifest;
    void* handle;                        // dlopen handle
    std::vector<std::string> registeredStrategies;
    std::vector<std::string> registeredIndicators;
    std::optional<core::Error> loadError;
};
```

Registrations from a plugin are tagged with the plugin's `id`, so:
- The Plugins tab can show "this plugin gave me these things".
- Disabling a plugin removes only its registrations.
- Two plugins registering the same `kind` causes the second to fail (`pluginIncompatibleAbi` with a clearer message), preserving determinism.

---

## 6. Trust model

Native plugins run **in-process** — they have full OS access. We do not pretend otherwise.

UI signals:

- The Plugins tab shows the file path and **SHA-256** of every loaded `.so`/`.dll`/`.dylib`.
- First-time load shows a **confirmation dialog** with the path + hash and asks the user to "Trust this plugin". The hash + decision is saved to `<userData>/config/trustedPlugins.json`. Subsequent loads with the same hash skip the prompt.
- Updating a plugin (different hash, same filename) re-prompts.

This is the same model as VSCode / Sublime extensions. We're explicit so users don't get surprised.

**Script strategies** (Lua today; Python per **`05`**/**`11`** when shipped) use **`bteStrategy`'s sandbox** instead — **no trust hash dialog** (`05`). **Natural language** drafts still require explicit user acceptance before they become executable artifacts (`05` §6).

---

## 7. Hot reload

- **Lua / Python** (tracked extensions): re-saving `.lua` or `.py` under `<userData>/strategies/` triggers `QFileSystemWatcher` and a recompile. Editor shows **reloaded** or compile error (`02`, `05`).
- **Native plugins**: hot reload is **not supported** in Phase 1. The user must restart the app. Reason: reliable unload of a `dlopen`'d C++ shared library requires meticulous management of static destructors and Qt meta-objects; not worth the surface area now.

---

## 8. Versioning the host's API

Lua and native plugins consume version stamps from **`bte::apiVersion`**; embedded **Python** strategies use **`pythonApiVersion`** once the host ships (`05`).

| Surface | Where it appears | Example |
|---|---|---|
| Lua | `bte.apiVersion` global | `"1"` |
| Python | module / metadata field `pythonApiVersion` | `"1"` |
| Native | `BTE_PLUGIN_ABI_MAJOR` macro | `1` |

The app publishes these in **Help → About** and on the Plugins tab alongside the app semver (`09` §6).

---

## 9. Sample plugin layout

```
sma200/
├── CMakeLists.txt
├── README.md
├── plugin.cpp
└── tests/
    └── smokeTest.cpp
```

`plugin.cpp` is < 200 lines and demonstrates strategy + indicator registration. We keep it building in CI on all three OSes — if it breaks, the SDK release is broken.

---

## 10. Tests

- Load a fixture plugin from `Tests/Unit/Plugins/Fixtures/`, assert its strategy is selectable.
- Plugin with mismatched `abiMajor`: load fails with the exact error code, app stays up.
- Plugin that registers a duplicate `kind`: second registration rejected, first still works.
- Plugin whose `bteRegisterPlugin` crashes: caught by the host (signal handler around `dlsym` calls) — plugin marked failed, app continues.
- SHA-256 trust prompt: first load prompts; same file again does not.
