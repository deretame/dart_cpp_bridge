---
title: Configuration
description: dart_cpp_bridge.yaml configuration file reference
sidebar:
  order: 1
---

## Configuration File

Every project that needs code generation requires a `dart_cpp_bridge.yaml` in the project root:

```yaml
# dart_cpp_bridge.yaml
dart_package: my_app

cpp_root: native/

scan:
  - native/api/

# Only include paths for the project itself. dart_cpp_bridge's native/include directory is
# resolved automatically from .dart_tool/package_config.json during codegen, so it does not need to be listed here.
include_paths:
  - native
  - native/api

dart_output: lib/src/native_gen/
cpp_wire_output: native/generated/

# Optional: candidate paths for clang-format (tried in order, falling back to PATH last).
# May be an executable path or a directory containing the executable.
# clang_format:
#   - C:\Program Files\LLVM\bin

std: c++20
defines:
  - BRIDGE_CODEGEN
  - DART_CPP_BRIDGE_CODEGEN
```

## Field Reference

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `cpp_root` | ✅ | `native/` | C++ source root directory; `scan` paths are resolved relative to it |
| `scan` | ✅ | - | List of directories to scan for annotated header files (relative to `cpp_root`) |
| `include_paths` | ✅ | `[]` | Project-relative include paths passed to clang |
| `dart_output` | ✅ | `lib/src/native_gen/` | Output directory for generated Dart code |
| `cpp_wire_output` | ✅ | `native/generated/` | Output directory for generated C++ wire dispatch code |
| `dart_package` | ❌ | Auto-read from `pubspec.yaml` | Dart package name, used for `assetId` and import paths; if set manually, it must match the `name` field in `pubspec.yaml` |
| `clang_format` | ❌ | None | Candidate path list for `clang-format`, used to format generated C++ code |
| `std` | ❌ | `c++20` | C++ standard used when parsing C++ headers |
| `defines` | ❌ | `BRIDGE_CODEGEN`, `DART_CPP_BRIDGE_CODEGEN` | Preprocessor macros passed to clang |
| `dart_code` | ❌ | None | Inject custom Dart code into generated data classes; can replace the auto-generated `toString()` |

## Running Code Generation

```bash
# Recommended: install to $DART_DATA_HOME/install/bin/ and use
dart install dcb_gen_tool
dcb_gen_tool generate dart_cpp_bridge.yaml

# Legacy global activate method (still works)
dart pub global activate dcb_gen_tool
dcb_gen_tool generate dart_cpp_bridge.yaml
```

The first run automatically downloads and caches the Python + libclang toolchain.

## include_paths Notes

:::tip
`include_paths` only needs to list the project's own header directories. `dart_cpp_bridge` runtime headers and `dcb_gen_tool` stub headers are resolved automatically during codegen; do not add them manually.
:::

## Header Organization Recommendations

`dcb_gen_tool` parses scanned headers with libclang — **including every header
they include transitively**. A header that libclang cannot resolve does not
always stop generation; it can silently degrade unresolved template types
(e.g. `std::vector<T>`, `std::unordered_map<K,V>`) to `int`, producing
bindings that only fail later at C++/Dart compile time.

### Include Whitelist

`native/api/*.h` may only include:

- C++ standard library headers (for signature types);
- `dart_cpp_bridge/*` runtime headers;
- `async_simple/coro/Lazy.h` for `BRIDGE_ASYNC` return types (backed by
  `dcb_gen_tool` stubs when the real dependency has not been fetched yet).

Do **not** include other third-party or dependency headers in scanned headers —
including headers that only exist in your build environment (`build/_deps`,
vendored SDKs, ...). Move heavy includes and implementations into
`native/api_impl/*.cpp`, which codegen never parses.

### Declaration Hygiene

- Headers scanned by `scan` should contain **declarations only**; do not put function implementations in them (do not paste `.cpp` content into the header).
- **Data classes** and **opaque classes** must be defined directly in the header; codegen only parses scanned headers and will not see classes defined elsewhere.
- Free functions, static methods, constructors, and other implementations should go in the corresponding `.cpp` files.
- **Do not write type aliases** (`using Foo = ...` or `typedef ...`); codegen currently cannot parse aliases, so expand them to the actual type.
- **Do not write `using namespace`**; always use fully qualified names for types and function calls (e.g. `std::int32_t`, `async_simple::coro::Lazy`).
- Function/method parameter and return types should use fully qualified names so codegen can identify them correctly.

## Custom Data Class `toString()` {#dart_code}

For data classes, codegen generates `hashCode`, `operator ==`, and `toString()` by default. If you want a data class to use a custom `toString()`, use `dart_code` to inject it:

```yaml
dart_code:
  Rect: |
    @override
    String toString() => 'Rect[$topLeft -> $bottomRight]';
```

Rules:
- The key is the data class name (must match the C++ class name marked with `BRIDGE_DATA_CLASS`).
- The injected code is written into the generated Dart class body as-is.
- When a class has `dart_code`, it **replaces** the auto-generated `toString()`, but `hashCode` and `operator ==` are still kept.

For more about data class field type restrictions, see [Type Mapping → Data Class](../type-mapping/#data-class).
