---
title: Channel 通道
description: 在 C++ sender 流水线中使用 oneshot 和 mpsc
---

Channel 是 C++ 侧的并发原语，用于让回调、worker 线程、流生产者和其他协程
交换值，而不阻塞 bridge io 线程。它不是 Dart Stream；如果结果要暴露为
Dart Stream，应使用 StreamSink。

引入头文件：

~~~cpp
#include <dart_cpp_bridge/channel.hpp>
#include <dart_cpp_bridge/runtime.hpp>
#include <stdexec/execution.hpp>
~~~

## oneshot

oneshot 最多传递一个值或一个错误。它的 receiver 自身就是 sender，所以
直接等待即可：

~~~cpp
BRIDGE_ASYNC
stdexec::task<std::string> wait_for_worker() {
  auto [tx, rx] = dcb::co::oneshot::channel<std::string>();

  exec::start_detached(dcb::spawn_blocking(
      [tx = std::move(tx)]() mutable {
        tx.send("finished");
      }));

  std::optional<std::string> value = co_await std::move(rx);
  co_return value.value_or("closed");
}
~~~

结果是 std::optional<T>：有值表示发送成功，std::nullopt 表示 receiver 在
收到值前已经关闭。send 不阻塞调用线程，可以从任意线程调用：

~~~cpp
auto [tx, rx] = dcb::co::oneshot::channel<int>();
bool accepted = tx.send(42);  // 已结算或关闭时返回 false
tx.close();                  // receiver 收到 nullopt
~~~

错误场景可以调用 send_error，传入 exception_ptr。等待中的 task 会收到
set_error，可以用 try/catch 或 upon_error 处理。oneshot receiver 是
move-only，应移动到 co_await 表达式中。

oneshot 有意不注入 stop token。销毁等待中的 receiver 会使它脱离；迟到的
send 会返回 false，不会向已经销毁的协程交付值。

## mpsc::unbounded

当生产者需要同步发送、消费者可以接受无界缓冲时，使用 unbounded：

~~~cpp
auto [tx, rx] = dcb::co::mpsc::unbounded<std::string>();

tx.send("first");  // 非阻塞；关闭后返回 false
tx.send("second");

while (auto item = co_await rx.recv()) {
  consume(*item);
}
~~~

mpsc::Receiver<T> 是单消费者。多个生产者可以复制 sender 并发 send，但
不能在同一个 receiver 上并发调用 recv。只有通道关闭并且缓冲值全部取完后，
receiver 才返回 std::nullopt。

最后一个 sender 析构时会关闭通道，也可以显式调用 tx.close()；receiver
析构也会关闭自己的这一侧。

## mpsc::bounded

需要背压时使用 bounded：

~~~cpp
auto [tx, rx] = dcb::co::mpsc::bounded<Frame>(64);

while (Frame frame = next_frame()) {
  bool accepted = co_await tx.send(std::move(frame));
  if (!accepted) {
    break;  // receiver 已关闭
  }
}
~~~

返回的 send sender 不会阻塞 OS 线程。容量有空位时立即完成；队列满时，
operation 会挂起，直到 receiver 腾出位置。co::mpsc::bounded<T>(0) 是
无缓冲 rendezvous 通道。

等待位置的 send 具有 cancel-safe 语义：receiver 认领值之前，值一直属于
send operation；operation 被停止或销毁时，值会被撤回，不会悄悄进入通道。

bounded 和 unbounded 的 receiver API 相同：

~~~cpp
if (auto item = co_await rx.recv()) {
  consume(*item);
}

if (auto item = rx.try_recv()) {
  consume(*item);
}
~~~

try_recv 只适合一次性的非阻塞查询；需要等待时仍应使用 recv。

## Stop token

Channel 的等待操作会使用 receiver environment 中的 stop token。直接组合
sender 时可以用 write_env 注入：

~~~cpp
auto [tx, rx] = dcb::co::mpsc::bounded<int>(1);
stdexec::inplace_stop_source stop_source;

auto send = stdexec::write_env(
    tx.send(1),
    stdexec::prop{
        stdexec::get_stop_token,
        stop_source.get_token()});

exec::start_detached(std::move(send));
stop_source.request_stop();
~~~

bounded send 被停止时通过 set_stopped 完成，并撤回尚未被认领的值；receive
被停止时不会丢弃已经缓冲的值。需要把取消转成普通值时，可以使用
stopped_as_optional，或在自定义 receiver 中处理 set_stopped。

完整的取消和竞态语义见
[docs/channel_stop_token_design.md](https://github.com/deretame/dart_cpp_bridge/blob/main/docs/channel_stop_token_design.md)。

## Channel 和 Dart API

Channel 适合 C++ 内部实现：

- oneshot 是 worker 结果回到等待 task 的常用桥梁；
- mpsc 适合生产者/消费者服务或流处理流水线；
- StreamSink<T> 是把值发送给 Dart 的另一套 API。

如果要暴露 Dart Stream，请查看[Streams 指南](/dart_cpp_bridge/zh-cn/guides/fundamentals/streams/)，
使用 BRIDGE 标记和 StreamSink 参数。不要把 channel 类型直接放进 codegen API
签名。

## 常见错误

- 对 oneshot receiver 调用 co_await rx.recv()。oneshot 本身就是 sender，
  应使用 co_await std::move(rx)；
- 在 io 线程调用 sync_wait 接收 channel；应该直接 await；
- 在一个 mpsc receiver 上同时启动两个 recv()；
- 在 operation 仍需要 receiver 或 sender 时提前销毁它；
- 误以为 channel send 会把数据复制给 Dart。它只在 C++ operation 之间移动
  C++ 值，只有生成 API 回传时才会经过 bridge codec。

