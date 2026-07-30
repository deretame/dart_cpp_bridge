---
title: Project Directory Structure
description: The directory layout of a typical dart_cpp_bridge project
---

`dcb_gen_tool init` generates a basic project skeleton. Knowing which files are hand-written and which are generated will help you avoid editing the wrong files.

## Typical Layout

```text
my_project/
├── dart_cpp_bridge.yaml      # codegen configuration
├── pubspec.yaml              # Dart/Flutter package configuration
├── hook/
│   └── build.dart            # Native Assets build hook
├── native/
│   ├── api/
│   │   └── my_api.h          # hand-written C++ API header (BRIDGE_* markers)
│   ├── api_impl/
│   │   └── my_api.cpp        # hand-written business implementation
│   ├── CMakeLists.txt        # hand-written/adjusted build configuration
│   └── generated/
│       ├── wire_dispatch.hpp # generated
│       └── wire_dispatch.cpp # generated
└── lib/
    ├── src/
    │   └── native_gen/
    │       ├── dcb_bindings.dart      # generated: FFI function signatures
    │       ├── dcb_generated.dart     # generated: internal implementation / singleton
    │       └── api/
    │           └── my_api.dart        # generated: top-level call entry points
    └── my_project.dart       # hand-written: exports public API
```

## Hand-written vs Generated

| Path | Type | Description |
|---|---|---|
| `dart_cpp_bridge.yaml` | Hand-written | Configures source dir, output directories, library name, etc. |
| `pubspec.yaml` | Hand-written | Dart/Flutter dependencies |
| `hook/build.dart` | Hand-written | Native Assets build entry |
| `native/api/*.h` | Hand-written | Bridge API headers containing `BRIDGE_*` markers |
| `native/api_impl/*.cpp` | Hand-written | Business implementation |
| `native/CMakeLists.txt` | Hand-written / adjusted | Builds the native library |
| `native/generated/*` | Generated | Wire dispatch routing |
| `lib/src/native_gen/*` | Generated | FFI bindings and Dart implementation |
| `lib/<project>.dart` | Hand-written | Exports the Dart API you want to expose |

## Generation Command

After modifying `native/api/*.h`, run:

```bash
dcb_gen_tool generate
```

Or from the `dcb_gen_tool` source directory:

```bash
cd dcb_gen_tool
dart run bin/dcb_gen.dart generate ../path/to/dart_cpp_bridge.yaml
```

## Notes

- Do not manually edit files in `native/generated/` and `lib/src/native_gen/`; they will be overwritten on re-generation.
- If the generated output paths are not what you want, adjust them in `dart_cpp_bridge.yaml`.
- Business code should go in `native/api_impl/` or `native/api/` so the generated layer does not depend on the concrete implementation.

## Further Reading

- [Code Generation Configuration](/dart_cpp_bridge/codegen/configuration/)
- [Code Generation Output](/dart_cpp_bridge/codegen/output/)
