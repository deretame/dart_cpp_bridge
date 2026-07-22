# Codegen（未实现）

Phase 2 计划：

1. `versions.lock` 钉死 **Python 解释器** 与 **libclang**（URL + hash，分平台）
2. 入口脚本每次启动校验 hash，失败则远端重下（**禁止**本机 Python/LLVM）
3. 解析 `bridge_api.h` → IR → 生成 Dart + C++ wire
4. **不**由 Native Assets hook 调用

当前请使用 `src/wire/demo_api.cpp` + `dart/lib` 手写绑定。
