---
title: Annotation Markers
description: BRIDGE_* macro markers in C++ header files
sidebar:
  order: 2
---

## Overview

Use annotation markers in C++ header files to tell the code generator how to handle functions and classes.

These macros expand to `__attribute__((annotate("bridge::*")))` when `BRIDGE_CODEGEN` is defined, and expand to nothing otherwise.

## Function Annotations

### BRIDGE_SYNC

Synchronous function that returns the result directly:

```cpp
BRIDGE_SYNC int32_t add(int32_t a, int32_t b);
```

### BRIDGE_ASYNC

Asynchronous function that returns `Lazy<T>`:

```cpp
BRIDGE_ASYNC async_simple::coro::Lazy<int32_t> compute_async(int32_t input);
```

### BRIDGE_NORMAL

Ordinary function that is dispatched to the thread pool for execution:

```cpp
BRIDGE_NORMAL std::string blocking_read(std::string path);
```

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
