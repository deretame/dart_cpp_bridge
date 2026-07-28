---
title: 已知问题
description: 已解决和已知的技术债
---

## 已解决

### CI codegen 容器类型退化

**现象**: CI 上生成的代码将 `std::vector` 等容器类型当作 `int` 处理。

**根因**: CI 在 cmake 之前运行 codegen，`build/_deps` 不存在，导致 libclang 解析失败，模板类型退化为 `int`。

**解决**: 补全 `dcb_gen_tool/stubs/async_simple/` 目录下的 stub 头文件。

### DartFn oneshot 通道

**现象**: DartFn 反向调用偶尔丢失响应。

**根因**: oneshot 通道在跨线程场景下的竞态条件。

**解决**: 使用 `asio::post` 确保回调在正确的线程执行。

## 已知限制

### 无取消机制

没有通用的异步取消。Stream 订阅取消只停止新事件的传递；C++ 侧继续运行并静默丢弃后续的 `add()` 调用。

### 无 ABI/API 稳定性

版本 `0.1.0-dev`。方法 ID、wire 格式、生成代码可能变化。

### DartFn 阻塞调用

`DartFn::operator()` 返回 `Lazy<Ret>`（仅异步）。阻塞场景使用 `syncAwait(dcb::spawn(fn(args...)))`。在 `io_context` 线程上调用 `syncAwait` 会自死锁。库不会自动卸载。

### codegen 不是构建步骤

代码生成必须在 API 头文件变更后手动运行。Native Assets hook（Phase 3）只会编译和链接，不会重新生成代码。

## 常见陷阱

:::danger
永远不要阻塞 `io_context` 线程。
:::

- 阻塞工作必须使用 `spawn_blocking` 或 `thread_pool`
- Runtime 是单线程设计，这是有意为之
- 生成的代码不是构建步骤，需要手动运行 codegen
