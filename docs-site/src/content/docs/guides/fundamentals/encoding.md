---
title: Wire Encoding and Runtime Codec
description: Frame structure, payload layout, and runtime encoding in dart_cpp_bridge
---

:::tip[Scope of this page]
This page documents binary frames, payload byte layout, and the Dart/C++ runtime
codec. For C++ to Dart types and the codegen whitelist, see
[Type Mapping](/dart_cpp_bridge/codegen/type-mapping/). For the complete fixed
frame specification, see [Wire Protocol](/dart_cpp_bridge/reference/wire-protocol/).
:::

## Two layers

Each call has two layers of data:

1. **Frame**: a fixed header routes requests, responses, streams, and DartFn messages.
2. **Payload**: generated code writes arguments or results in declaration order.

The flow is:

```text
Dart generated API
        │  arguments / result
        ▼
payload (ByteWriter / ByteReader)
        │  plus the 24-byte fixed header
        ▼
wire frame
        │
        ▼
C++ dispatch / Session
```

You normally do not write `ByteWriter` or `ByteReader` directly. Codegen creates
the C++ dispatch and Dart codec; application code only provides annotated C++ APIs.

## Frame header

The fixed header is 24 bytes, encoded in little-endian order:

| Offset | Size | Field | Purpose |
|---:|---:|---|---|
| 0 | 4 | `magic` | `0x31424344` (`DCB1`) |
| 4 | 2 | `version` | currently `1` |
| 6 | 1 | `msg_type` | request, response, stream, or DartFn |
| 7 | 1 | `flags` | reserved, currently `0` |
| 8 | 8 | `request_id` | RPC, stream, or DartFn reply correlation ID |
| 16 | 4 | `method_id` | generated API method number |
| 20 | 4 | `payload_len` | payload size in bytes |
| 24 | N | `payload` | argument or result data |

See [Wire Protocol](/dart_cpp_bridge/reference/wire-protocol/) for message types
and compatibility constraints. `method_id`, field order, and protocol version
are compatibility contracts and must not be changed casually.

## Primitive payload encoding

Payloads have no self-describing field names or type tags. The sender and
receiver must use the same generated code and read/write in the same order:

| Wire value | Size / layout |
|---|---|
| `bool` | 1 byte, `0` or `1` |
| `u8` | 1 byte |
| `i32` / `u32` | 4 bytes, little-endian |
| `i64` | 8 bytes, little-endian |
| `f32` / `f64` | IEEE 754 floating point |
| `string` | `u32` byte length + UTF-8 bytes, without `NUL` |
| `DateTime` | `i64` Unix microsecond timestamp, interpreted as UTC |
| `Int128` / `UInt128` | `u32` length + decimal ASCII string |
| internal `u64` | frame IDs, object handles, pointer addresses; not a public codegen primitive |

The public C++ to wire-value mapping is defined by [Type Mapping](/dart_cpp_bridge/codegen/type-mapping/).
The fact that the low-level codec can carry `i16`, `u16`, or `u64` does not make
those values public codegen API types.

## Composite values

- `std::vector<T>`: a `u32` element count, followed by each element.
- `std::vector<uint8_t>`: the same `u32` count, followed by the raw bytes.
- `std::array<T, N>`: exactly `N` elements, with **no count prefix**; generated
  Dart code checks the length.
- `std::optional<T>`: a `u8` presence tag; `0` is `None`, and `1` is followed by `T`.
- `std::map` / `std::unordered_map`: a `u32` count, followed by each key/value.
- `std::set` / `std::unordered_set`: a `u32` count, followed by each element.
- `std::pair` / `std::tuple`: positional values, with no length or field names.
- `enum class`: the enum value encoded as `i32`.
- `BRIDGE_DATA_CLASS`: recursively encoded in C++ field declaration order, with no field names.
- `BRIDGE_OPAQUE`: an `u64` object handle, not object fields.
- `std::uint8_t*` / `const std::uint8_t*`: only the `u64` native address; pointed-to
  bytes and length are not included. Pass the length as a separate argument.

Ordered and unordered map/set types have the same payload shape. Iteration order
of an unordered container is not part of the wire protocol semantics.

## Rules for hand-written codecs

Direct codec use is for hand-written wire dispatch, test fixtures, or the
runtime itself. Writes and reads must be paired exactly:

```cpp
dcb::ByteWriter writer;
writer.i32(42);
writer.str("Hi");

const auto& raw = writer.raw();
dcb::ByteReader reader(raw.data(), raw.size());
const auto number = reader.i32();  // 42
const auto text = reader.str();    // "Hi"
```

In a normal project, generated `wire_dispatch.cpp` and Dart `*.g.dart` perform
this work. Do not wrap business functions in another custom Future/Stream or
manually assemble payloads. Async functions use `stdexec::task<T>`, streams use
`dcb::StreamSink<T>`, and the generator keeps the types and encoding aligned.

## Payload example

These values are encoded in order:

```text
int32 42       →  2A 00 00 00
string "Hi"    →  02 00 00 00 48 69
optional<int32>(42)
               →  01 2A 00 00 00
```

For an API argument `std::vector<uint8_t>{10, 20}`, the payload fragment is:

```text
02 00 00 00 0A 14
```

`std::array<int32_t, 2>{10, 20}` has no `02 00 00 00` count prefix because the
array length is already part of the C++ type.

## Error frames

C++ exceptions do not cross FFI. At the wire boundary they become a
`responseErr` frame:

```text
code      i32     error code (generated dispatch currently uses 1)
message   string  length-prefixed error message
```

Dart turns the error frame into an exception; callers never need to parse a C++
exception object.

## Validation and lifetime

- `parseFrame` / `ByteReader` validate magic, protocol version, payload length,
  and read boundaries.
- The wire protocol is for Dart and bundled C++ in one process, not for parsing
  untrusted network input.
- `uint8_t*` transmits only an address. Dart-owned input memory must remain valid
  until a synchronous call returns or an async `Future` completes; C++ must
  validate the address and length itself.
- Ownership and validity of returned pointers are part of the C++ API contract.
  The bridge does not copy, free, or add bounds checks automatically.

## Further reading

- [Type Mapping](/dart_cpp_bridge/codegen/type-mapping/): C++ types, Dart types, and codegen rules
- [Wire Protocol](/dart_cpp_bridge/reference/wire-protocol/): frame fields and message types
- [stdexec Usage Guide](/dart_cpp_bridge/guides/fundamentals/stdexec/): sender/task patterns for async code
- [Code Generation](/dart_cpp_bridge/codegen/): generate Dart/C++ bindings from C++ headers
