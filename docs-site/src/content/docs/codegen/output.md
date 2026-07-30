---
title: Output Files
description: File structure produced by the code generator
sidebar:
  order: 4
---

## C++ Side

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

```text
lib/src/generated/
├── api_fn.dart    # Top-level functions (recommended entry point)
├── api.dart       # BridgeApi singleton
└── api.g.dart     # BridgeApiImpl (method IDs, codec)
```

### Three-Layer Structure

| Layer | File | Purpose |
|---|---|---|
| Top-level functions | `api_fn.dart` | `initBridge()`, `add()`, ... |
| Singleton | `api.dart` | `BridgeApi.instance` |
| Implementation | `api.g.dart` | Method IDs, codec logic |

### Usage Example

```dart
import 'package:my_app/src/generated/api_fn.dart';

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
