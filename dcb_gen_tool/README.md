# dcb_gen_tool

Code generation CLI for [dart_cpp_bridge](https://github.com/deretame/dart_cpp_bridge).

Parses annotated C++ headers via libclang and generates Dart/C++ bridge code
(sync, async, stream, DartFn reverse calls, opaque classes).

## Installation

```bash
dart pub global activate dcb_gen_tool
```

Requires Dart SDK >= 3.5.0. No Python, LLVM, or Rust installation needed —
the tool downloads a pinned, hash-verified Python toolchain automatically.

## Quick start

```bash
# 1. (First run only) Download the toolchain (~100 MB, cached for future use)
dcb_gen bootstrap

# 2. Generate bridge code for your project
cd my_project
dcb_gen generate dart_cpp_bridge.yaml
```

## Commands

| Command | Description |
|---------|-------------|
| `dcb_gen generate <config.yaml>` | Run the full codegen pipeline (parse C++ → generate Dart + C++ wire) |
| `dcb_gen bootstrap` | Download and verify the pinned Python + libclang toolchain |
| `dcb_gen doctor` | Check environment health (Dart SDK, toolchain cache, CMake) |

## Options

| Option | Description |
|--------|-------------|
| `--force` | Force re-download even if cached toolchain is valid |
| `--quiet` | Suppress non-error output |
| `--version` | Print version and exit |
| `--help` | Show usage |

## Configuration

The `dart_cpp_bridge.yaml` config file tells the tool which headers to parse
and where to output generated code. Example:

```yaml
library_name: demo
headers:
  - native/api/bridge_api.h
  - native/api/counter.h
include_paths:
  - native/api
dart_output: lib/src/native_gen
cpp_output: native/generated
```

## How it works

1. **Bootstrap**: Downloads [python-build-standalone](https://github.com/astral-sh/python-build-standalone)
   and [libclang-ng](https://pypi.org/project/libclang-ng/) wheels, verifies
   SHA-256 checksums, and caches them in a platform-specific directory:
   - Windows: `%LOCALAPPDATA%\dart_cpp_bridge\toolchain`
   - macOS: `~/Library/Caches/dart_cpp_bridge/toolchain`
   - Linux: `~/.cache/dart_cpp_bridge/toolchain`

2. **Parse**: Uses libclang (via Python) to parse C++ headers and extract
   functions/classes annotated with `BRIDGE_SYNC`, `BRIDGE_ASYNC`,
   `BRIDGE_NORMAL`, `BRIDGE_OPAQUE`, `BRIDGE_DATA_CLASS` markers.

3. **Generate**: Emits:
   - **Dart API layer** (`api/*.dart`) — thin user-facing functions/classes
   - **Dart impl layer** (`gcm_generated.dart`) — codec, wire dispatch, lifecycle
   - **C++ wire dispatch** (`wire_dispatch.hpp/.cpp`) — frame routing, scheduling
   - **IR** (`ir.json`) — intermediate representation for debugging

## C++ annotation markers

```cpp
#include <dart_cpp_bridge/annotate.h>

BRIDGE_SYNC int32_t add(int32_t a, int32_t b);
BRIDGE_ASYNC Lazy<std::string> fetch_data(std::string url);
BRIDGE_NORMAL void tick(StreamSink<int32_t> sink);

BRIDGE_OPAQUE class Counter {
public:
  BRIDGE_SYNC static Counter* new_with_initial_value(int32_t v);
  BRIDGE_ASYNC int32_t value();
};
```

## Environment override

| Variable | Description |
|----------|-------------|
| `DCB_CODEGEN_CACHE` | Override toolchain cache directory |

## License

MIT
