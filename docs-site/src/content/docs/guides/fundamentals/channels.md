---
title: Channels
description: oneshot and mpsc channels for C++ sender pipelines
---

Channels are C++-side primitives. They let a callback, worker thread, stream
producer, or another coroutine exchange values without blocking the bridge io
thread. They are not Dart Stream objects; use a StreamSink when the result must
be exposed as a Dart Stream.

Include the channel header:

~~~cpp
#include <dart_cpp_bridge/channel.hpp>
#include <dart_cpp_bridge/runtime.hpp>
#include <stdexec/execution.hpp>
~~~

## oneshot

An oneshot channel carries at most one value or one error. Its receiver is
itself a sender, so await it directly:

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

The result is std::optional<T>: an engaged value means send succeeded, and
std::nullopt means the channel was closed before a value arrived. Sending is
non-blocking and safe from any thread:

~~~cpp
auto [tx, rx] = dcb::co::oneshot::channel<int>();
bool accepted = tx.send(42);  // false if already settled or closed
tx.close();                  // completes the receiver with nullopt
~~~

For an error, call send_error with an exception pointer. The awaiting task then
receives set_error and can handle it with a try/catch or upon_error. An
oneshot receiver is move-only and should be moved into the await operation.

An oneshot channel intentionally does not inject a stop token. Destroying the
pending receiver detaches it; a late send then returns false instead of
delivering into a destroyed coroutine.

## mpsc::unbounded

Use an unbounded channel when producers should send synchronously and the
consumer can tolerate an unbounded buffer:

~~~cpp
auto [tx, rx] = dcb::co::mpsc::unbounded<std::string>();

tx.send("first");  // non-blocking; false only after close
tx.send("second");

while (auto item = co_await rx.recv()) {
  consume(*item);
}
~~~

mpsc::Receiver<T> is single-consumer. Multiple producers may copy the sender
and call send concurrently, but do not call recv concurrently on the same
receiver. The receiver returns std::nullopt only after the channel is closed
and the buffered values have been drained.

The last sender destructor closes the channel. You can close it explicitly
with tx.close(); the receiver destructor also closes its side.

## mpsc::bounded

Use a bounded channel to apply backpressure:

~~~cpp
auto [tx, rx] = dcb::co::mpsc::bounded<Frame>(64);

while (Frame frame = next_frame()) {
  bool accepted = co_await tx.send(std::move(frame));
  if (!accepted) {
    break;  // receiver closed
  }
}
~~~

The returned send sender never blocks the calling OS thread. If capacity is
available it completes immediately; when the queue is full it parks the
operation until a receiver frees a slot. co::mpsc::bounded<T>(0) creates a
rendezvous channel with no buffer.

A parked send is cancel-safe: until a receiver claims the value, it remains in
the send operation state. If the operation is stopped or destroyed, the value
is withdrawn and is not silently inserted into the channel.

The receiver API is the same for bounded and unbounded channels:

~~~cpp
if (auto item = co_await rx.recv()) {
  consume(*item);
}

if (auto item = rx.try_recv()) {
  consume(*item);
}
~~~

Use try_recv only for a point-in-time, non-blocking poll. It does not replace
recv for a coroutine that needs to wait.

## Stop tokens

Channel wait operations use the stop token from their receiver environment.
Inject one with write_env when testing or composing a sender directly:

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

A stopped bounded send completes through set_stopped and withdraws its value.
A stopped receive leaves already buffered values untouched. For a task that
must turn cancellation into a normal value, wrap the operation with
stopped_as_optional or handle set_stopped in a custom receiver.

The exact cancellation and race semantics are specified in
[docs/channel_stop_token_design.md](https://github.com/deretame/dart_cpp_bridge/blob/main/docs/channel_stop_token_design.md).

## Channels and Dart APIs

A channel is useful inside C++ implementation code:

- oneshot is the normal bridge between a worker result and an awaiting task;
- mpsc is useful for a producer/consumer service or a stream pipeline;
- StreamSink<T> is the separate API for sending values to Dart.

For Dart-facing streams, use the [Streams guide](/dart_cpp_bridge/guides/fundamentals/streams/)
and a BRIDGE marker plus StreamSink parameter. Do not expose a channel type in a
codegen API signature.

## Common mistakes

- Calling co_await rx.recv() on an oneshot receiver. Oneshot is already a
  sender; use co_await std::move(rx).
- Calling sync_wait to receive from the io thread. Await the channel instead.
- Starting two recv() operations concurrently on one mpsc receiver.
- Destroying the receiver or sender while an operation still needs it.
- Assuming a channel send copies data to Dart. It only moves C++ values between
  C++ operations; the bridge codec is involved only when a generated API
  response is posted.

