import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';
import 'package:test/test.dart';

/// Smoke test for the @Native binding path.
///
/// `DartCppBridge.init()` with no `libraryPath` resolves the runtime through
/// `@Native` externals backed by the code asset built by `hook/build.dart`
/// (a runtime-only shared library, no wire dispatch registered). It exercises
/// `dcb_init_dart_api` / `dcb_set_pool_threads` / `dcb_session_open` on the way
/// in and `dcb_session_close` / `dcb_shutdown` on the way out.
void main() {
  test('@Native runtime session smoke (no libraryPath)', () async {
    final bridge = await DartCppBridge.init();
    expect(bridge, isNotNull);
    expect(identical(DartCppBridge.instance, bridge), isTrue);
    // Stop the process-wide runtime and close the session.
    bridge.shutdown();
  }, skip: 'Requires dart/hook/build.dart (removed; source-only distribution)');
}
