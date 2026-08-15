---
title: Known Issues
description: Known limitations and common pitfalls in the current version
slug: v1/reference/known-issues
---

## Known Limitations

### No General Cancellation Mechanism

There is no general async cancellation. Cancelling a `Stream` subscription only stops new events from being delivered; the C++ side continues running and silently drops subsequent `add()` calls.

### Codegen Is Not a Build Step

Code generation must be run manually after API header changes. The Native Assets hook only handles compilation and linking; it does not regenerate code.

### Type Aliases Are Not Supported

codegen cannot parse `using Foo = ...` or `typedef ...`. Use the actual type directly in headers and write out the full namespace.

### Opaque Class Limitations

* Cannot be shared across Isolates
* Inheritance, virtual functions, and method overloading are not supported
* Field access requires hand-written getter/setter methods

## Common Pitfalls

:::danger
Never block the `io_context` thread.
:::

* Blocking work must use `spawn_blocking` or be posted to the `thread_pool`
* `set_pool_threads()` must be called before `Runtime::start()`
* The Runtime is single-threaded by design
* Generated code must be regenerated manually by running `dcb_gen_tool generate`
* Headers should contain only declarations; data classes and opaque classes must be defined inside the scanned headers
* `DartFn::operator()` is async only; for blocking calls use `syncAwait(dcb::spawn(...))`, and never do so on the io thread
