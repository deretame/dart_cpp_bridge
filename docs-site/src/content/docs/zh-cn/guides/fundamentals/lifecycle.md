---
title: 生命周期管理
description: Runtime、Session、Opaque 对象与 NativeFinalizer 的生命周期
---

:::note[v2.1.0]
本页使用 `IoContextScheduler`；`AsioExecutor` 是 v1 名称。Session、Dart 和
finalizer 生命周期规则在两代中共用。
:::

bridge 的生命周期分三层：进程级 Runtime、Isolate 级 Session、对象级 Opaque handle。理解它们谁创建、谁释放、谁不能动，是避免死锁和内存泄漏的关键。

## Runtime：进程级单例

`dcb::Runtime` 是进程级单例，内部管理：

- `asio::io_context` 事件循环
- `IoContextScheduler`（stdexec scheduler）
- `thread_pool`
- Session 注册表

### 启动与停止

- **启动**：通常由 `DartCppBridge.init()` 自动触发；C++ 单测或纯 C++ 程序可以手动调用 `dcb::Runtime::instance().start()`
- **停止**：`DartCppBridge.shutdown()` 或 `dcb::Runtime::instance().stop()`

**限制**：`shutdown()` 只能在**主 isolate / 进程退出**时调用，会关闭所有 Session 并停止 Runtime。

## Session：每个 Isolate 一个

每个调用 `DartCppBridge.init()` 的 Dart Isolate 拥有一个 Session：

- 独立的 reply port
- 独立的 `DartFn` 闭包注册表
- 生成计数 `generation`，用于丢弃 `dispose()` 后的迟到消息

### Session 的创建与关闭

| 操作 | 调用位置 | 行为 |
|---|---|---|
| `init()` | 任意 Isolate | 创建或复用 Session |
| `dispose()` | 当前 Isolate | 立即关闭该 Isolate 的 Session |
| isolate 关闭 / GC | 任意 | `NativeFinalizer` 自动关闭 Session |
| `shutdown()` | 主 isolate 退出 | 关闭所有 Session |

### 规则

- `dispose()` 是可选的，日常依赖 `NativeFinalizer` 即可
- `shutdown()` 不要在 worker isolate 中调用
- worker isolate 可以 `init()`，拥有自己的 Session，但不能 `shutdown()`

## Opaque 对象：per-Session handle

`BRIDGE_OPAQUE` 标记的 C++ 对象通过 handle 在 Dart 侧引用：

- 构造时：C++ 创建对象，注册到 `ObjectHandleRegistry`，返回 handle
- 使用时：Dart 传 handle 给 C++ 实例方法
- 销毁时：Dart GC 触发 `NativeFinalizer` → `dcb_drop_object` → 从 registry 删除并析构

### 生命周期边界

- Session 关闭时，该 Session 下所有 Opaque 对象自动释放
- 如果 Dart 侧仍持有对象引用但 Session 已关闭，后续调用会失败

## 典型流程

```text
App 启动
  └─ main isolate 调用 DartCppBridge.init()
       └─ Runtime 启动（如未启动）
       └─ 创建 Session A
  ├─ worker isolate 调用 DartCppBridge.init()
  │    └─ 创建 Session B
  │
  ├─ Dart 调用 C++ 创建 Opaque 对象
  │    └─ ObjectHandleRegistry 注册，返回 handle
  │
  ├─ Dart GC 或 dispose 释放 Opaque
  │    └─ NativeFinalizer → dcb_drop_object → 析构
  │
  └─ App 退出
       └─ main isolate 调用 shutdown()
            └─ 关闭 Session A / B，停止 Runtime
```

## 常见错误

| 错误 | 后果 |
|---|---|
| worker isolate 调 `shutdown()` | 会关闭主 isolate 的 Session，Runtime 停止，bridge 失效 |
| `dispose()` 后继续调用 | 该 isolate 的调用会失败 |
| 持有 Opaque 对象跨 Isolate | 对象不能跨 Isolate 共享 |
| 在 `BRIDGE_SYNC` 里调 DartFn | 死锁（Dart 回复需要 io 线程） |

## 延伸阅读

- [函数标记选择指南](/dart_cpp_bridge/guides/fundamentals/markers/)
- [架构设计](/dart_cpp_bridge/guides/fundamentals/architecture/)
