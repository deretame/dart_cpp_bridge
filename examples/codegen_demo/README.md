# codegen_demo — Project Template

This directory serves as both the Phase 2 end-to-end test fixture and a **copyable user project template**.

To integrate `dart_cpp_bridge` with your own C++ library, start here: copy this directory, modify the config and headers, run codegen, build the DLL, and call from Dart.

For full codegen documentation, see [dcb_gen_tool/README.md](../../dcb_gen_tool/README.md).

---

## Quick Start (after copying this template)

```text
1. Copy this directory to your project
2. Modify dart_cpp_bridge.yaml (scan paths, include_paths, output paths)
3. Write BRIDGE_* annotated headers in native/api/
4. Write C++ implementations in native/api_impl/
5. Run codegen (generates wire + Dart bindings)
6. Build the shared library with CMake
7. Import the generated API from Dart and call
```

---

## Directory Structure

```text
examples/codegen_demo/
  dart_cpp_bridge.yaml       # codegen config (you modify this)
  native/
    api/                     # BRIDGE_* declarations (scan input, you modify)
      bridge_api.h           # Top-level function API
      counter.h              # Opaque class API
      point_rect.h           # Data class API
    api_impl/                # User implementations (you modify)
      bridge_api.cpp
      counter.cpp
    generated/               # codegen output (do not edit)
      wire_dispatch.hpp/cpp  # C++ dispatch
      ir.json                # Intermediate representation
  lib/
    main.dart                # Flutter app shell (runs bridge tests on device)
    codegen_demo.dart        # Exports generated API
    src/native_gen/          # codegen Dart output (do not edit)
      api.g.dart             # BridgeApiImpl (low-level)
      api.dart               # BridgeApi.instance (singleton)
      api_fn.dart            # Top-level functions (recommended)
  test/api_test.dart         # End-to-end tests (flutter test)
  integration_test/          # Android on-device integration tests
  android/                   # Flutter Android host app
  CMakeLists.txt             # Builds shared library
```

---

## 1. Configure `dart_cpp_bridge.yaml`

```yaml
cpp_root: native/            # C++ project root

scan:                        # Only scan .h/.hpp in these directories
  - native/api/

include_paths:               # -I paths for libclang parsing
  - native
  - native/api
  - ../../include            # dart_cpp_bridge public headers
  - ../../dcb_gen_tool/stubs      # async_simple stub

dart_output: lib/src/native_gen/   # Dart output
cpp_wire_output: native/generated/ # C++ wire output

std: c++20
defines:
  - BRIDGE_CODEGEN
  - DART_CPP_BRIDGE_CODEGEN
```

**For your project**: change `include_paths` to point to your dart_cpp_bridge repository path (or FetchContent path).

---

## 2. Write API Headers

Create `.h` files in `native/api/` with `BRIDGE_*` markers:

```cpp
#pragma once
#include "dart_cpp_bridge/annotate.h"
#include <async_simple/coro/Lazy.h>
#include <cstdint>
#include <string>

namespace my_api {

// sync → Dart: int bridgeVersion()
BRIDGE_SYNC
std::int32_t bridge_version();

// async → Dart: Future<int> add(int a, int b)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int32_t> add(std::int32_t a, std::int32_t b);

// normal (thread pool) → Dart: Future<String> sleepGreeting(String name)
BRIDGE_NORMAL
std::string sleep_greeting(std::string name);

}  // namespace my_api
```

See [docs/codegen_type_mapping.md](../../docs/codegen_type_mapping.md) for supported types.

---

## 3. Run Codegen

```bash
cd <repo>/codegen
dart pub get
dart run bin/codegen.dart scripts/run_codegen.py ../examples/codegen_demo/dart_cpp_bridge.yaml
```

First run downloads pinned Python + libclang-ng to user cache (see codegen README).

Generated files:

| Path | Content |
|------|---------|
| `native/generated/wire_dispatch.*` | C++ dispatch |
| `native/generated/ir.json` | IR |
| `lib/src/native_gen/api.g.dart` | Low-level impl |
| `lib/src/native_gen/api.dart` | Singleton facade |
| `lib/src/native_gen/api_fn.dart` | Top-level functions |

**Type errors**: If headers use types outside the whitelist, codegen reports clear errors (file:line + hint) and does not generate broken code.

---

## 4. Build Native Library

**First** build the main project at repo root (or have `build/_deps` available). The demo CMake reuses asio / async-simple to avoid re-cloning.

```powershell
# Repo root (if _deps not yet available)
cmake -S . -B build
cmake --build build --config Release

# Demo
cd examples\codegen_demo
cmake -S . -B build
cmake --build build --config Release
```

Output: `build/Release/dcb_codegen_demo.dll` (or `.so` / `.dylib`).

The DLL contains: runtime + `ffi_entry` + **generated wire** + user `api_impl` (**not** the main project's hand-written `demo_api.cpp`).

### External Project Integration (FetchContent)

If your project is outside the dart_cpp_bridge repo, use CMake FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(
  dart_cpp_bridge
  GIT_REPOSITORY https://github.com/deretame/dart_cpp_bridge.git
  GIT_TAG        main   # or pin to tag/commit
)
FetchContent_MakeAvailable(dart_cpp_bridge)

# dart_cpp_bridge target PUBLIC exposes asio + async-simple
target_link_libraries(my_bridge PRIVATE dart_cpp_bridge)
```

> Note: FetchContent integration is not yet productionized (Phase 3). Currently recommended within monorepo or manually specify `DCB_ROOT`.

---

## 5. Test

This is a Flutter package (it includes an Android host app). Use `flutter test` instead of `dart test`:

```powershell
cd examples\codegen_demo
flutter pub get
flutter test
```

### Android integration tests

The same bridge tests also run on a real device/emulator via `integration_test/`:

```bash
# Requires a connected device or running emulator (x86_64/arm64).
# The Native Assets hook cross-compiles the native library with the NDK.
flutter test integration_test
```

Coverage:

| Marker | C++ | Dart top-level call | Expected |
|--------|-----|---------------------|----------|
| `BRIDGE_SYNC` | `bridge_version` | `bridgeVersion()` | `42` |
| `BRIDGE_ASYNC` | `add` | `await add(2, 3)` | `5` |
| `BRIDGE_NORMAL` | `sleep_greeting` | `await sleepGreeting('Ada')` | `hello, Ada` |
| `BRIDGE_EXPORT` data class | `distance(Point, Point)` | `await distance(...)` | `double` |
| `BRIDGE_EXPORT` opaque class | `Counter` | `Counter.withInitialValue(...)` | instance methods |

Unmarked `internal_helper` does **not** appear in generated output.

---

## 6. Usage from Dart

Recommended (top-level functions):

```dart
import 'package:codegen_demo/codegen_demo.dart';

await initBridge(libraryPath: r'...\dcb_codegen_demo.dll');
print(bridgeVersion());
print(await add(1, 2));
print(await sleepGreeting('world'));
shutdownBridge(); // only on process exit
```

Singleton equivalent:

```dart
await BridgeApi.instance.init(libraryPath: '...');
BridgeApi.instance.bridgeVersion();
```

---

## 7. Customization Checklist

After copying this template, typically modify:

| File | What to change |
|------|----------------|
| `dart_cpp_bridge.yaml` | `scan` paths, `include_paths`, output paths |
| `native/api/*.h` | Your API declarations |
| `native/api_impl/*.cpp` | Your business implementations |
| `CMakeLists.txt` | Target name, source files, `DCB_ROOT` path |
| `pubspec.yaml` | Package name, dependencies |
| `lib/codegen_demo.dart` | Export paths |

**Do not modify**:
- `native/generated/` — codegen overwrites automatically
- `lib/src/native_gen/` — codegen overwrites automatically
- `include/dart_cpp_bridge/` — bridge public headers, reference directly

---

## 8. Header Conventions

```cpp
// native/api/bridge_api.h
BRIDGE_SYNC   std::int32_t bridge_version();
BRIDGE_ASYNC  async_simple::coro::Lazy<std::int32_t> add(std::int32_t a, std::int32_t b);
BRIDGE_NORMAL std::string sleep_greeting(std::string name);
```

Implementations go in `native/api_impl/`. Changing implementations does **not** require re-running codegen; only changing signatures or adding new exported APIs does.
