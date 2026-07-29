# dcb_gen_tool

Code generation CLI for [dart_cpp_bridge](https://github.com/deretame/dart_cpp_bridge).

Parses annotated C++ headers via libclang and generates Dart/C++ bridge code
(sync, async, stream, DartFn reverse calls, opaque classes).

## Installation

```bash
dart pub global activate dcb_gen_tool
```

Requires Dart SDK >= 3.10.0. No Python, LLVM, or Rust installation needed —
the tool downloads a pinned, hash-verified Python toolchain automatically.

## Quick start

```bash
# 1. Create a Dart project with dart_cpp_bridge dependency, then:
cd my_project
dart pub get

# 2. Scaffold the bridge project (config + CMake + hook + example API)
dcb_gen init

# 3. Run — the Native Assets hook builds C++ automatically
dart run
```

`dcb_gen init` generates a fully working example (header + implementation +
CMake + hook). You can `dart run` immediately, then replace the example API
with your own functions.

## Commands

| Command | Description |
|---------|-------------|
| `dcb_gen init` | Scaffold a new bridge project (config, CMake, hook, example API + impl) |
| `dcb_gen generate <config.yaml>` | Run the full codegen pipeline (parse C++ → generate Dart + C++ wire) |
| `dcb_gen bootstrap` | Download and verify the pinned Python + libclang toolchain |
| `dcb_gen doctor` | Check environment health (Dart SDK, toolchain cache, CMake) |

### `dcb_gen init` details

Generates the following files (skips any that already exist):

```
dart_cpp_bridge.yaml        # codegen config
native/CMakeLists.txt       # CMake build (auto-resolves dart_cpp_bridge package)
native/api/bridge_api.h     # example C++ header with BRIDGE_* annotations
native/api_impl/bridge_api.cpp  # example implementation (ready to run)
hook/build.dart             # Native Assets build hook
```

Options: `--name <lib>` (auto-reads from `pubspec.yaml` if omitted).

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
dart_package: my_app
cpp_root: native/

scan:
  - native/api/

include_paths:
  - native
  - native/api

dart_output: lib/src/native_gen/
cpp_wire_output: native/generated/

# Optional: clang-format candidate paths (tried top-to-bottom, then PATH).
clang_format:
  - C:\Program Files\LLVM\bin

std: c++20
defines:
  - BRIDGE_CODEGEN
  - DART_CPP_BRIDGE_CODEGEN
```

| Field | Description |
|-------|-------------|
| `dart_package` | Dart package name (must match `pubspec.yaml` name) |
| `cpp_root` | Root directory for C++ sources |
| `scan` | Directories to scan for annotated headers |
| `include_paths` | Project-relative include paths for clang parsing |
| `dart_output` | Output directory for generated Dart code |
| `cpp_wire_output` | Output directory for generated C++ wire dispatch |
| `clang_format` | Optional list of clang-format paths (dir or executable) |
| `std` | C++ standard (default `c++20`) |
| `defines` | Preprocessor defines passed to clang |
| `dart_code` | Optional custom Dart code injected into data class bodies |

Optionally inject custom Dart code into generated data class bodies (e.g. a
custom `toString()`); when present for a type it replaces the auto-generated
`toString()`:

```yaml
dart_code:
  Rect: |
    @override
    String toString() => 'Rect[$topLeft -> $bottomRight]';
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
   `BRIDGE_NORMAL`, `BRIDGE_OPAQUE`, `BRIDGE_DATA_CLASS`, `BRIDGE_TO_STRING`
   markers.

3. **Generate**: Emits:
   - **Dart API layer** (`api/*.dart`) — thin user-facing functions/classes
   - **Dart impl layer** (`dcb_generated.dart`) — codec, wire dispatch, lifecycle
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
