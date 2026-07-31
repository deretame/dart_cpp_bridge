---
title: Output Files
description: File structure produced by the code generator
sidebar:
  order: 4
---

## C++ Side

Defaults to `native/generated/` (configured via `cpp_wire_output` in `dart_cpp_bridge.yaml`):

```text
native/generated/
├── wire_dispatch.hpp   # Dispatch function declarations
├── wire_dispatch.cpp   # Dispatch implementation (frame decoding, method routing, response encoding)
└── ir.json             # Intermediate representation (for debugging)
```

### wire_dispatch.cpp

Generated dispatch code handles:
- Frame decoding (`ByteReader`)
- Method routing (`switch (method_id)`)
- Parameter deserialization
- Calling business functions
- Return value serialization
- Error capture and encoding

## Dart Side

Defaults to `lib/src/native_gen/` (configured via `dart_output` in `dart_cpp_bridge.yaml`):

```text
lib/src/native_gen/
├── api/
│   ├── init.dart              # init / dispose / BridgeApi singleton
│   ├── bridge_api.dart        # corresponds to native/api/bridge_api.h
│   ├── counter.dart           # corresponds to native/api/counter.h
│   ├── foreign_api.dart       # corresponds to native/api/foreign_api.h
│   └── multi_runtime_api.dart # corresponds to native/api/multi_runtime_api.h
├── dcb_bindings.dart          # FFI native symbol bindings
└── dcb_generated.dart         # Method IDs, shared codec, internal implementation
```

In other words: `native/api/{name}.h` generates `lib/src/native_gen/api/{name}.dart`.

### Entry Point

Import the package's root export file:

```dart
import 'package:codegen_demo/codegen_demo.dart';
```

`lib/codegen_demo.dart` re-exports everything under `api/`.

### Three-Layer Structure

Each generated Dart API file still follows the three-layer pattern internally:

| Layer | Location | Purpose |
|---|---|---|
| Top-level functions | `api/{name}.dart` | `initBridge()`, `add()`, ... |
| Singleton | `api/init.dart`, etc. | `BridgeApi.instance` |
| Implementation | `dcb_generated.dart` | Method IDs, codec logic |

### Usage Example

```dart
import 'package:my_app/codegen_demo.dart';

void main() async {
  await initBridge();

  final result = await add(1, 2);
  print(result); // 3

  disposeBridge();
}
```

## Business Code

Business implementation stays in user-written files:

```text
native/api_impl/bridge_api.cpp  # User-written implementation
```

The code generator does not modify these files.
