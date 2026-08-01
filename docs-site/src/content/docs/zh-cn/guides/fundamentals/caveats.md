---
title: 注意事项与常见坑
description: "集中收录常见问题：codegen 约束、线程规则、取消语义、生命周期，以及 C / 外部运行时契约"
---

本页集中收录容易踩坑的注意事项。每条都链到完整说明；详细内容保留在各自章节，
本页只做轻量索引，方便快速查找。

## 代码生成与 API 头文件

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 被扫描的 API 头文件 include 了三方库或依赖头 | libclang 会解析所有传递包含的头；解析不了时会把模板类型（`std::vector`、`std::unordered_map` 等）静默降级为 `int`，生成的绑定到编译时才报错 | 只允许 include C++ 标准库、`dart_cpp_bridge/*` 和 `async_simple/coro/Lazy.h`；重型 include 放到 `native/api_impl/*.cpp`。参见 [include 白名单](../../codegen/configuration/#include-白名单) |
| 在 API 头文件里写实现 | 生成层依赖实现细节，头文件变得脆弱 | 只放声明，实现进 `api_impl/*.cpp`。参见 [声明规范](../../codegen/configuration/#声明规范) |
| 在扫描头里写类型别名 / `using namespace` | codegen 无法解析别名，可能生成错误的类型 | 展开别名，使用完整限定名 |
| 手改 `native/generated/` 或 `lib/src/native_gen/` | 下次生成会覆盖 | 当作构建产物，不要编辑。参见 [项目结构](../fundamentals/project-structure/) |
| 以为构建 hook 会自动重新生成代码 | Native Assets hook 只负责编译和链接，不会重新生成 | API 头文件改动后手动运行 `dcb_gen_tool generate`。参见 [Native Assets 构建 hook](../fundamentals/native-assets-hooks/) |

## 线程与死锁

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 阻塞 io 线程 | 整个事件循环卡死，所有协程和回调都停摆 | 阻塞工作交给 `spawn_blocking` / 线程池。参见 [线程规则](../fundamentals/runtime/#线程规则) |
| 在 io 线程上 `syncAwait` | 自死锁：被等待的协程需要 io 线程恢复 | 只在非 io 线程调用 `syncAwait` |
| 在 `BRIDGE_SYNC` 里调用 `DartFn` | 死锁：Dart 的回复要通过 io 线程投递 | 改用异步标记（`BRIDGE_ASYNC` / `BRIDGE_NORMAL`）或 `spawn_blocking`。参见 [常见错误](../fundamentals/markers/#常见错误) |
| 使用 `RescheduleLazy::detach()` | 异常在 io 线程上抛出，直接崩溃进程 | 使用 `dcb::spawn_detached` |
| MSVC 下用协程 lambda | 挂起恢复后捕获的变量被破坏 | 改用静态协程函数或显式传参。参见 [常见错误](../fundamentals/async-simple/#常见错误) |

## 取消与 Stream

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 指望 Dart 强制取消 Future | Dart 的 `Future` 只是监听者，无法打断正在运行的 C++ 协程 | 提供协作式取消：按 task id 注册 `async_simple::Signal`，通过 `cancelTask` 一类的 API 发出 `Terminate`。参见 [取消（Signal & Slot）](../fundamentals/async-simple/#取消signal-slot) |
| `sleep()` 取消后不醒 | 没有绑定信号时，定时器会一直跑到超时 | 用 `Lazy::setLazyLocal(signal)` 绑定协程链，`sleep()` 才可打断。参见 [可取消的 sleep](../fundamentals/async-simple/) |
| `collectAll` / `collectAny` 不传 `Terminate` | 第一个任务完成后，失败方/慢任务还在跑 | 用 `collectAll<Terminate>` / `collectAny<Terminate>` 取消输家。参见 [取消输家](../fundamentals/async-simple/) |
| 取消 Stream 订阅 | 只停止 Dart 侧接收；C++ 侧继续跑，迟到的 `add()` 被静默丢弃 | 显式停止生产者，或接受 fire-and-forget 语义。参见 [Stream](../fundamentals/markers/) |

## 生命周期与会话

| 坑 | 后果 | 正确做法 |
|---|---|---|
| worker isolate 调用 `shutdown()` | 关掉主 isolate 的 Session 并停止 Runtime | 只有主 isolate 能 shutdown。参见 [生命周期管理](../fundamentals/lifecycle/) |
| 跨 isolate 共享 Opaque 对象 | handle 是 per-session 的，其他 isolate 无法使用 | 对象留在所属 isolate 内 |
| `dispose()` 之后继续调用 | 调用失败，session 已不存在 | 重新 init，或先完成工作再 dispose |

## 纯 C API（cbridge）

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 在 `dcb_invoke_dart_fn` 回调里阻塞 | 回调在 bridge 的 io 线程触发，阻塞会卡死事件循环 | 只做少量工作，然后转交给其他线程。参见 [行为](../../guides/advanced/cbridge/) |
| 完成/取消后复用 async op | `op_id` 是一次性的，后续调用都是 no-op | 每次操作创建新的 op。参见 [异步操作原语](../../guides/advanced/cbridge/) |
| 指望纯 C 代码 `co_await` | C 侧只负责创建/完成/取消 op，等待端是 C++ 协程 | 在 `Lazy` 里通过 `dcb::async_wait` 桥接 |

## 外部运行时

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 在非 loop 线程调用定时器 API | libuv / glib 的定时器 API 不是线程安全的 | `schedule_after` 保证在 loop 线程调用，不要破坏这个约定 |
| `cancel_after` 直接操作定时器 | 与 loop 线程竞争 | 把取消转发到 loop 线程；对已触发或未知 handle 要安全 no-op。参见 [定时器流程](../advanced/foreign-runtime/) |
| 还有挂起协程时就 unregister | 已挂起的协程不会被恢复 | 先关闭 channel / 结束任务再 unregister。参见 [Worker 的契约](../advanced/foreign-runtime/) |

## 延伸阅读

- [代码生成配置](../../codegen/configuration/)
- [async-simple 协程入门](../fundamentals/async-simple/)
- [内置运行时](../fundamentals/runtime/)
