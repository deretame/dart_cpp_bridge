---
title: Versioned Documentation
description: Choose the v1 released documentation or the v2 stdexec development documentation
---

`dart_cpp_bridge` currently has two C++ async generations. This site uses the
Starlight Versions plugin to keep the current v2 documentation at the root and
the released v1 documentation under `/v1/`.

## Choose a version

| Version | Status | Async C++ model | URL |
| --- | --- | --- | --- |
| [v1 — released 1.x](/dart_cpp_bridge/v1/) | Published; current package line is 1.3.0 | async-simple `Lazy` / `Executor` | `/v1/` |
| v2 — development | Next major version; not published yet | stdexec senders / schedulers / `stdexec::task` | `/` (current) |

Use the version selector in the site header to switch versions. The unmarked
guides at the site root describe v2. The archived v1 pages remain available for
maintenance work, but are not instructions for new v2 code.

The Dart API, C ABI, wire protocol, generated file layout, and method ID rules
remain stable contracts. The migration primarily changes C++ async business
signatures and scheduler integration.

For repository-level details, see
[`docs/versioning.md`](https://github.com/deretame/dart_cpp_bridge/blob/main/docs/versioning.md).
