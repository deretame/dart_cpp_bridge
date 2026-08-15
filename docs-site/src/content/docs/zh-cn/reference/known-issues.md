---
title: 已知问题
description: 当前版本已知限制与常见陷阱
---

:::caution[v2 开发线]
本列表按当前 stdexec 实现编写。历史 async-simple 问题保留在 v1 归档中，不是
当前 API 的使用指导。
:::

## 已知限制

### 无通用取消机制

没有通用的异步取消。Stream 订阅取消只停止新事件的传递；C++ 侧继续运行并静默丢弃后续的 `add()` 调用。

### codegen 不是构建步骤

代码生成必须在 API 头文件变更后手动运行。Native Assets hook 只负责编译和链接，不会重新生成代码。

### 不支持类型别名

codegen 无法解析 `using Foo = ...` 或 `typedef ...`。头文件中请直接使用实际类型并写完整命名空间。

### 不透明类限制

- 不能跨 Isolate 共享
- 不支持继承、虚函数、方法重载
- 字段访问需手写 getter/setter

## 常见陷阱

:::danger
永远不要阻塞 `io_context` 线程。
:::

- 阻塞工作必须使用 `spawn_blocking` 或投递到 `thread_pool`
- `set_pool_threads()` 必须在 Runtime `start()` 之前调用
- Runtime 是单线程设计，这是有意为之
- 生成的代码需要手动运行 `dcb_gen_tool generate` 重新生成
- 头文件应只放声明，数据类和不透明类必须定义在被扫描的头文件内
- `DartFn::operator()` 仅异步；阻塞调用从 worker 或外部线程使用
  `dcb::sync_wait(...)`，且禁止在 io 线程上执行
