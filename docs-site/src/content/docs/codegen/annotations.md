---
title: Annotation Markers
description: BRIDGE_* macro markers in C++ header files
sidebar:
  order: 2
---

:::caution[v2 development line]
For `BRIDGE_ASYNC`, v2 headers return `stdexec::task<T>` or another supported
stdexec sender. Published v1 projects should use the [version guide](/dart_cpp_bridge/versions/)
before copying an async declaration.
:::

## Overview

Use annotation markers in C++ header files to tell the code generator how to handle functions and classes.

These macros expand to `__attribute__((annotate("bridge::*")))` when `BRIDGE_CODEGEN` is defined, and expand to nothing otherwise.

## Important naming constraints

All functions and opaque-class methods exported to Dart must have **globally unique qualified names** within the scanned API surface. C++ function overloading is **not supported**.

### Why

The bridge derives a stable integer method ID for each exported API by hashing its fully-qualified C++ name. The Dart side dispatches calls using that ID. If two functions share the same qualified name, the generator cannot distinguish them, and Dart has no equivalent of C++ overload resolution.

### Rules

- Do not declare two `BRIDGE_SYNC`, `BRIDGE_ASYNC`, or `BRIDGE_NORMAL` functions with the same qualified name.
- Opaque-class methods must also be unique within their class. Two methods named `process` on the same `Counter` class are not allowed, even with different signatures.
- Rename overloaded C++ functions before exposing them to the bridge. For example:

  ```cpp
  BRIDGE_SYNC int32_t add_ints(int32_t a, int32_t b);
  BRIDGE_SYNC double add_doubles(double a, double b);
  ```

Violating this constraint causes the code generator to report a duplicate-function error and stop.

## Function Annotations

### BRIDGE_SYNC

Synchronous function that returns the result directly:

```cpp
BRIDGE_SYNC int32_t add(int32_t a, int32_t b);
```

### BRIDGE_ASYNC

Asynchronous function that returns `stdexec::task<T>` or another supported sender:

```cpp
#include <stdexec/execution.hpp>
BRIDGE_ASYNC stdexec::task<int32_t> compute_async(int32_t input);
```

### BRIDGE_NORMAL

Ordinary function that is dispatched to the thread pool for execution:

```cpp
BRIDGE_NORMAL std::string blocking_read(std::string path);
```

### Stream Functions

A function with a required `dcb::StreamSink<T>` parameter is generated as a Dart `Stream<T>`,
but only when it also carries an export marker (`BRIDGE_SYNC` / `BRIDGE_ASYNC` /
`BRIDGE_NORMAL`). For plain `void` stream functions use `BRIDGE_NORMAL`:

```cpp
BRIDGE_NORMAL
void tick_stream(dcb::StreamSink<int32_t> sink, int32_t count);
```

Constraints:

- The export marker is the gate: a `StreamSink` parameter alone does not export the function
  (the generator warns and skips it)
- Optional streams use `std::optional<dcb::StreamSink<T>>` on `BRIDGE_ASYNC` /
  `BRIDGE_NORMAL` / `BRIDGE_SYNC` functions instead (sync events are delivered after the FFI
  call returns)

### BRIDGE_PERSIST

Marks a function with DartFn parameters as a persistent callback: the Dart side does not automatically unregister the closure after the call, allowing C++ to store and invoke it repeatedly. Typically used with `BRIDGE_SYNC` (registration) or `BRIDGE_NORMAL` (trigger):

```cpp
BRIDGE_SYNC
BRIDGE_PERSIST
bool register_dart_fn(dcb::DartFn<std::string(std::string)> callback);
```

Constraints:

- The function must contain at least one `dcb::DartFn` parameter
- Callbacks are not cleaned up automatically; the caller must manage their lifecycle

## Class Annotations

### BRIDGE_DATA_CLASS

Pure data class (fields only, no exported methods):

```cpp
struct BRIDGE_DATA_CLASS Point {
  double x;
  double y;
};
```

Constraints:

- No inheritance
- No virtual functions
- No `BRIDGE_SYNC/ASYNC/NORMAL` methods

### BRIDGE_OPAQUE

Opaque class (methods only, public fields are ignored):

```cpp
class BRIDGE_OPAQUE Counter {
 public:
  BRIDGE_SYNC void increment();
  BRIDGE_SYNC int32_t value() const;
 private:
  int32_t count_ = 0;
};
```

### BRIDGE_TO_STRING

Marks an opaque-class method as the source for Dart `toString()`:

```cpp
class BRIDGE_OPAQUE Widget {
 public:
  BRIDGE_SYNC BRIDGE_TO_STRING std::string to_string() const;
};
```

Constraints:

- Must be a synchronous instance method
- No arguments
- Returns `std::string`

## Aliases

All `BRIDGE_*` macros have `DCB_*` aliases:

```cpp
DCB_SYNC == BRIDGE_SYNC
DCB_ASYNC == BRIDGE_ASYNC
DCB_DATA_CLASS == BRIDGE_DATA_CLASS
// ...
```
