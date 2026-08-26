# Dart API DL headers

Headers/sources from Dart SDK `runtime/include` (tag used by
`cmake/fetch_dart_api.cmake`). Native CMake downloads them automatically into
the build tree when this directory does not contain a complete copy.

To refresh a source-tree copy manually:

```bash
cmake -P cmake/fetch_dart_api.cmake
```
