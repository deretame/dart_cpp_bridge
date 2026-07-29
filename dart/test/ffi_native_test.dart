import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';
import 'package:test/test.dart';

/// Smoke test for the @Native binding path.
///
/// This test requires a hook/build.dart that registers the code asset.
/// Since dart_cpp_bridge is now a source-only distribution (no bundled hook),
/// this test is skipped. Downstream packages (e.g. codegen_demo) exercise
/// the full @Native path via their own hooks.
void main() {
  test('@Native runtime session smoke', () async {
    // This would require a NativeBindings instance from @Native externals,
    // which only works when a hook registers the code asset.
    // See examples/codegen_demo for a working end-to-end test.
  }, skip: 'Requires a downstream hook to register the code asset');
}
