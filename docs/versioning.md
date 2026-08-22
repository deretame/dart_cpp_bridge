# Versioned documentation

`dart_cpp_bridge` has two C++ concurrency generations. v1 remains available for
existing 1.x applications, while the current v2.1.0 line is published from the
stdexec migration.

## At a glance

| Documentation version | Release status | C++ async model | Use it when |
| --- | --- | --- | --- |
| **v1** | Released 1.x; current published line is 1.3.0 | `async_simple::coro::Lazy`, `Executor`, `Signal` / `Slot` | Maintaining an existing v1 application or generated output |
| **v2** | Released 2.1.0 | stdexec senders, schedulers, `stdexec::task`, and stop tokens | New applications and migrated v2 applications |

The Dart public API, C ABI, wire protocol, generated file layout, and method
ID rules remain compatibility contracts in both generations. The main v2
source change is the C++ async business-code surface.

The public documentation site uses the `starlight-versions` plugin: v2 is the
current documentation at the site root, while v1 is archived under `/v1/` with
the version selector provided by the plugin.

## v1 — released 1.x

The v1 runtime is the model shipped by the 1.x releases:

- async functions return `async_simple::coro::Lazy<T>`;
- scheduling is expressed through `async_simple::Executor` and `.via(...)`;
- cooperative cancellation uses `Signal` / `Slot`;
- foreign event loops use `ForeignExecutor` / `foreign_runtime.h`;
- codegen recognizes the async-simple coroutine return type.

Use the v1 documentation when the application is pinned to a published 1.x
package. The async-simple page in the documentation site is intentionally kept
as a v1 archive.

## v2 — released 2.1.0 line

The current branch has completed the async-simple to stdexec migration:

- async functions return `stdexec::task<T>` or another stdexec sender;
- the built-in Asio loop is exposed as an `stdexec::scheduler`;
- use `stdexec::starts_on`, `stdexec::on`, `stdexec::continues_on`, and
  `stdexec::sync_wait` with their current names;
- cancellation uses `inplace_stop_source` / `stop_token`;
- foreign event loops provide a plain stdexec scheduler, as shown by
  `examples/foreign_runtime_demo/native/uv_scheduler.hpp`;
- generated async dispatch uses a zero-capture coroutine IIFE so lazy coroutine
  state is owned by the coroutine frame;
- `dcb_gen_tool` parses `stdexec::task` and emits the v2 dispatch shape.

The v2 implementation is published as `dart_cpp_bridge: ^2.1.0` and
`dcb_gen_tool: ^2.1.0`. Keep the two package versions aligned and regenerate
code with the matching tool version.

## Migration checklist

| v1 | v2 |
| --- | --- |
| `async_simple::coro::Lazy<T>` | `stdexec::task<T>` or a sender |
| `Executor` / `.via(ex)` | scheduler + `starts_on` / `on` |
| `syncAwait(lazy)` | `stdexec::sync_wait(sender)` |
| `collectAll` / `collectAny` | `when_all` / `exec::when_any` |
| `Signal` / `Slot` | stop source / stop token |
| `ForeignExecutor` | user-provided stdexec scheduler |

For the v2 usage rules and compile-verified examples, see the full
[`English reference`](./cpp26_executor_model_usage.en.md) or the full
[`Chinese reference`](./cpp26_executor_model_usage.md).
