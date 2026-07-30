---
title: C++ ↔ Dart Type Translation
description: A quick-reference table mapping C++ types to Dart types, plus the wire encoding rules behind them.
---

This page is a **type translation cheat sheet**: when you write a C++ API, you should know what type appears on the Dart side; when you call generated functions from Dart, you should also know what types C++ actually sends and receives. For the full rules, see [Type Mapping](/dart_cpp_bridge/codegen/type-mapping/) and [Wire Protocol](/dart_cpp_bridge/reference/wire-protocol/).

## C++ ↔ Dart Types

| C++ Type | Dart Type | Encoding |
|---|---|---|
| `bool` | `bool` | 1 byte (0/1) |
| `int8/16/32/64_t` | `int` | 1/2/4/8 bytes LE |
| `uint8/16/32/64_t` | `int` | 1/2/4/8 bytes LE |
| `float` | `double` | 4 bytes IEEE 754 |
| `double` | `double` | 8 bytes IEEE 754 |
| `std::string` | `String` | u32 length + UTF-8 bytes |
| `std::chrono::system_clock::time_point` | `DateTime` | i64 microseconds timestamp |
| `Int128` / `UInt128` | `BigInt` | length-prefixed decimal string |

## Containers

| C++ Type | Dart Type | Encoding |
|---|---|---|
| `std::vector<uint8_t>` | `Uint8List` | u32 count + bytes |
| `std::vector<int32_t>` | `Int32List` | u32 count + elements |
| `std::vector<float>` | `Float32List` | u32 count + elements |
| `std::vector<double>` | `Float64List` | u32 count + elements |
| `std::vector<T>` (other) | `List<T>` | u32 count + N elements |
| `std::optional<T>` | `T?` | 1 byte tag (0=None, 1=Some) + T |
| `std::unordered_map<K, V>` | `Map<K, V>` | u32 count + (K, V) pairs |
| `std::unordered_set<T>` | `Set<T>` | u32 count + N elements |
| `std::pair<T1, T2>` | `(T1, T2)` | encoded in order |
| `std::tuple<T1, T2, ...>` | `(T1, T2, ...)` | encoded in order |

## Enums

```cpp
enum class BRIDGE_EXPORT OrderStatus : std::int32_t {
  kCreated = 0,
  kPaid = 1,
};
```

- Must be marked with `BRIDGE_EXPORT`
- Underlying type must be `std::int32_t`
- Each constant must be explicitly assigned
- Wire-encoded as `int32`

## Data Classes

```cpp
struct BRIDGE_DATA_CLASS Point {
  double x;
  double y;
};
```

- Encoded field-by-field in **declaration order**
- Field names are not transmitted
- Inheritance, virtual functions, and methods are not supported

## Opaque Classes

`BRIDGE_OPAQUE` objects are passed as handles on the wire, not field-by-field encoded.

## Encoding Conventions

- **Little-endian**: all multi-byte integers are little-endian
- **Order-sensitive**: the Reader is a sequential cursor; read order must exactly match write order
- **Strings are not NUL-terminated**: `std::string` transmits u32 length + bytes, without `\0`
- **No field names**: data classes transfer values only, not field names

## Error Frames

Payload of `responseErr`:

```text
code    i32    currently always 0
message string error message
```

## Further Reading

- [Type Mapping](/dart_cpp_bridge/codegen/type-mapping/)
- [Wire Protocol](/dart_cpp_bridge/reference/wire-protocol/)
- [Pure C Bridge API](/dart_cpp_bridge/guides/advanced/cbridge/)
