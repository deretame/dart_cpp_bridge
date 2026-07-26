import 'package:hook_demo/hook_demo.dart';
import 'package:test/test.dart';

void main() {
  test('hookDemoAdd adds two integers', () {
    expect(hookDemoAdd(2, 3), 5);
    expect(hookDemoAdd(-1, 1), 0);
    expect(hookDemoAdd(0, 0), 0);
  });

  test('hookDemoSub subtracts two integers', () {
    expect(hookDemoSub(10, 4), 6);
    expect(hookDemoSub(0, 7), -7);
  });

  test('hookDemoMul multiplies two integers', () {
    expect(hookDemoMul(6, 7), 42);
    expect(hookDemoMul(-3, 5), -15);
  });
}
