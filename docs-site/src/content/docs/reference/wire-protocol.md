---
title: Wire Protocol
description: FFI binary frame format specification
---

## Frame Format

All Dart ↔ C++ communication uses little-endian binary frames:

```text
Offset  Size  Field       Description
0       4     magic       0x31424344 ('DCB1')
4       2     version     Protocol version (currently 1)
6       1     msg_type    Message type
7       1     flags       Reserved (0)
8       8     request_id  RPC ID / Stream ID / DartFn Reply ID
16      4     method_id   Method identifier
20      4     payload_len Payload length
24      N     payload     Payload data
```

## Message Types

| Value | Name | Description |
|---|---|---|
| 1 | `kRequest` | Dart → C++ request |
| 2 | `kResponseOk` | C++ → Dart success response |
| 3 | `kResponseErr` | C++ → Dart error response |
| 4 | `kStreamData` | Stream data frame |
| 5 | `kStreamEnd` | Stream end |
| 6 | `kStreamErr` | Stream error |
| 7 | `kDartFnCall` | C++ → Dart closure call |

## Error Encoding

Error response payload format:

```text
code      i32     Error code
message   string  Error message (length-prefixed)
```

Generated dispatch currently uses error code 1. The field remains part of the
protocol; callers should not depend on the formatting of the error text.

## Type Encoding

### Primitive Types

| Type | Encoding |
|---|---|
| `bool` | 1 byte (0/1) |
| `u8` | 1 byte |
| `i32` / `u32` | 4 bytes little-endian |
| `i64` | 8 bytes little-endian |
| `f32` | 4 bytes IEEE 754 |
| `f64` | 8 bytes IEEE 754 |
| `string` | u32 length + UTF-8 bytes (no NUL terminator) |
| `DateTime` | i64 Unix microsecond timestamp (UTC, no timezone) |
| `Int128` / `UInt128` | u32 length + decimal ASCII string; Dart handles conversion to `BigInt` |

Internal frame fields, object handles, and `uint8_t*` addresses use `u64`. The
codec can represent `i16`, `u16`, and `u64`, but they are not public codegen
primitive types at present.

### Enums

`enum class` with underlying type `i32`, encoded as `i32`.

### Container Types

| Type | Encoding |
|---|---|
| `std::vector<T>` | u32 count + T[]; `vector<uint8_t>` is followed by raw bytes |
| `std::array<T, N>` | exactly N values, **no count prefix** |
| `std::map<K, V>` / `std::unordered_map<K, V>` | u32 count + (K, V)[] |
| `std::set<T>` / `std::unordered_set<T>` | u32 count + T[] |
| `std::pair<T1, T2>` | T1 + T2 |
| `std::tuple<T1, T2, ...>` | T1 + T2 + ... (in position order) |
| `std::optional<T>` | u8 tag (1 = Some, 0 = None) + T (only if Some) |

`std::uint8_t*` / `const std::uint8_t*` encode only the native address (`u64`);
they do not encode pointed-to bytes or a length. The length must be passed by
the API separately. Data classes use declaration order, while `BRIDGE_OPAQUE`
values encode only their object handle.

## Security Considerations

:::caution
The wire protocol is not designed to parse untrusted data from the network. It is only for communication between Dart and bundled C++ code.
:::

- The codec validates `magic`, `version`, and payload length.
- Truncated or malformed frames throw an exception and are encoded as error frames.
- C++ exceptions are caught at the wire boundary and do not cross FFI.
