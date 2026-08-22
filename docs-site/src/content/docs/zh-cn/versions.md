---
title: 文档版本说明
description: 选择已发布的 v1 文档或 stdexec v2.1.0 文档
---

`dart_cpp_bridge` 目前有两代 C++ 异步模型。本网站使用 Starlight Versions
插件：当前 v2.1.0 文档位于根路径，已发布的 v1 文档归档在 `/v1/` 下。

## 选择版本

| 版本 | 状态 | C++ 异步模型 | URL |
| --- | --- | --- | --- |
| [v1 — 已发布的 1.x](/dart_cpp_bridge/zh-cn/v1/) | 已发布，当前发布线为 1.3.0 | async-simple `Lazy` / `Executor` | `/zh-cn/v1/` |
| v2.1.0 — 已发布 | 当前发布线 | stdexec sender / scheduler / `stdexec::task` | `/zh-cn/`（当前） |

请使用网站顶部的版本选择器切换版本。根路径下没有标记版本的指南默认描述
v2；v1 页面仅用于维护旧项目，不应作为新 v2 代码的写法参考。

Dart API、C ABI、wire 协议、生成文件布局和 method ID 规则仍是稳定契约，
迁移主要改变 C++ 异步业务函数签名和 scheduler 接入方式。

仓库级版本说明见
[`docs/versioning.zh-CN.md`](https://github.com/deretame/dart_cpp_bridge/blob/main/docs/versioning.zh-CN.md)。
