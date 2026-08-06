# dart_cpp_bridge 已知问题与技术债

> 更新日期：2026-08-06（ID-017 追加补充说明：spawn_on_io 改回模板 + sender_in 泛型约束但 OOM 不复发的原因；新增 ID-021：uv 并发测试偶发挂起观察；目录同步）
>
> **维护规则（每次编辑本文件必须遵守）：**
> 1. **更新日期**：每次增改内容后，把头部「更新日期」改成当天日期（YYYY-MM-DD）。
> 2. **同步目录**：新增 / 修改 / 删除任何条目后，同步更新下方「目录」（含子目录）。
> 3. **全只读、只追加**：本文件中所有已写下的内容（现象、分析、结论、方案、验证、状态）**一律只读**，只能追加新内容，禁止修改、改写或删除任何历史内容。哪怕发现之前某条结论是错的，也不能回头改它，只能在其后**追加一节说明**当前判断有误。
> 4. **状态也是追加**：「状态」字段同样不可修改，状态变化以追加「状态记录」行或「修复记录」节的形式呈现，最新一条即当前状态。

---

## 目录

- [1. 模板（参考）](#1-模板参考)
  - [1.1 维护规则说明](#11-维护规则说明)
  - [1.2 条目模板（复制即用）](#12-条目模板复制即用)
  - [1.3 模板示例](#13-模板示例)
- [2. 已知问题与已解决记录](#2-已知问题与已解决记录)
  - [2.1 运行时 / 会话](#21-运行时--会话)
    - [2.1.1 [ID-001] 问题标题](#211-id-001-问题标题)
    - [2.1.2 [ID-002] oneshot settle/start 发布顺序竞态（mpsc send 假失败、recv 假关闭、值丢失）](#212-id-002-oneshot-settlestart-发布顺序竞态mpsc-send-假失败recv-假关闭值丢失)
    - [2.1.3 [ID-003] recv 中途取消丢值（stale pending_tx）与 oneshot detached 语义](#213-id-003-recv-中途取消丢值stale-pending_tx与-oneshot-detached-语义)
    - [2.1.4 [ID-004] mpsc 单消费者契约的运行期强制](#214-id-004-mpsc-单消费者契约的运行期强制)
  - [2.2 构建 / 工具链](#22-构建--工具链)
    - [2.2.1 [ID-005] hook 增量构建缓存损坏导致「莫名崩溃」（0xC0000005）](#221-id-005-hook-增量构建缓存损坏导致莫名崩溃0xc0000005)
    - [2.2.2 [ID-006] pubspec.yaml description 单行含冒号未加引号导致 YAML 解析失败](#222-id-006-pubspecyaml-description-单行含冒号未加引号导致-yaml-解析失败)
    - [2.2.3 [ID-007] exec::task 编译期陷阱：无默认模板参数 / 无环境下不满足 stdexec::sender 概念](#223-id-007-exectask-编译期陷阱无默认模板参数--无环境下不满足-stdexecsender-概念)
    - [2.2.4 [ID-008] UV_HANDLE_CLOSING 是 libuv 内部宏，公共代码应使用 uv_is_closing()](#224-id-008-uv_handle_closing-是-libuv-内部宏公共代码应使用-uv_is_closing)
    - [2.2.5 [ID-013] hook 依赖监控缺失——native/ 外的头文件修改不触发重建](#225-id-013-hook-依赖监控缺失native-外的头文件修改不触发重建)
    - [2.2.6 [ID-016] stdexec FetchContent 的 execution.bs 下载失败留 0 字节文件 → CMake VERSION "0..0"](#226-id-016-stdexec-fetchcontent-的-executionbs-下载失败留-0-字节文件--cmake-version-00)
    - [2.2.7 [ID-017] 生成 wire 的 spawn_on_io 模板链在 MSVC 上峰值 >150 GB](#227-id-017-生成-wire-的-spawn_on_io-模板链在-msvc-上峰值-150-gb)
    - [2.2.8 [ID-018] exec::task 内 co_await DartFn 触发 continues_on 包装 → MSVC 错误风暴 + ~140 GB 提交内存](#228-id-018-exectask-内-co_await-dartfn-触发-continues_on-包装--msvc-错误风暴--140-gb-提交内存)
  - [2.3 其它](#23-其它)
    - [2.3.1 [ID-009] MSVC 19.51 协程 lambda 捕获损坏（co_await 后按值捕获变量变垃圾值）](#231-id-009-msvc-1951-协程-lambda-捕获损坏co_await-后按值捕获变量变垃圾值)
    - [2.3.2 [ID-010] 协程内持有全局锁跨 co_await 阻塞 io 线程（并发请求死锁）](#232-id-010-协程内持有全局锁跨-co_await-阻塞-io-线程并发请求死锁)
    - [2.3.3 [ID-011] uv_scheduler 并发设计陷阱（timer double-close / cancel 挂起 / WorkState 双重管理）](#233-id-011-uv_scheduler-并发设计陷阱timer-double-close--cancel-挂起--workstate-双重管理)
    - [2.3.4 [ID-012] dispose() UAF——on_async 在 run() 后触碰已析构的嵌入式 opstate（0xC0000005）](#234-id-012-dispose-uafon_async-在-run-后触碰已析构的嵌入式-opstate0xc0000005)
    - [2.3.5 [ID-014] start 队列 pop 窗口竞态与 unlink tail-orphan（claim 基类化修复）](#235-id-014-start-队列-pop-窗口竞态与-unlink-tail-orphanclaim-基类化修复)
    - [2.3.6 [ID-015] teardown TOCTOU 与 stop() 时挂起 timer 泄漏](#236-id-015-teardown-toctou-与-stop-时挂起-timer-泄漏)
    - [2.3.7 [ID-019] stdexec `inplace_stop_source::request_stop()` 返回值语义与 `std::stop_source` 相反（cancel_task 契约回归）](#237-id-019-stdexec-inplace_stop_sourcerequest_stop-返回值语义与-stdstop_source-相反cancel_task-契约回归)
    - [2.3.8 [ID-020] stdexec 迁移中 cbridge DartFn 回调契约回归（错误改抛异常 / 丢失 "C:" 前缀）](#238-id-020-stdexec-迁移中-cbridge-dartfn-回调契约回归错误改抛异常--丢失-c-前缀)
    - [2.3.9 [ID-021] codegen_demo 全量集成测试偶发挂起（libuv multiple concurrent requests 超时 5 分钟）](#239-id-021-codegen_demo-全量集成测试偶发挂起libuv-multiple-concurrent-requests-超时-5-分钟)

---

## 1. 模板（参考）

### 1.1 维护规则说明

| 要求 | 做法 |
|------|------|
| **更新日期** | 每次编辑后更新文件头部「更新日期」，并注明本次改动（可选，如 `（新增 ID-003；追加 ID-001 第 2 次修复）`）。 |
| **目录 + 子目录** | 目录按 `章节 → 分类 → 条目` 三级组织；每次新增条目时同步在目录中加入对应链接。 |
| **全只读、只追加** | 任何已写内容（含错误的结论）**永不修改、不删除**。新信息一律以「修复记录」或「补充说明」追加在条目末尾。 |
| **结论错误怎么处理** | 不得回头改原文。在条目末尾追加一节「补充说明（YYYY-MM-DD）」：指出原文哪句结论不对、当前正确结论是什么、依据（测试 / 代码 / 复现）。 |
| **状态只追加** | 「状态记录」只允许在末尾追加新行，不允许改动已有行；最新一行即当前状态。状态从「未解决」变为「已解决」时，追加一行而非修改旧行。 |
| **编号唯一** | 每条问题分配 `[ID-XXX]` 顺序编号，新问题递增编号，避免重排已有编号。 |

### 1.2 条目模板（复制即用）

```markdown
### [ID-XXX] 问题标题（一句话概括现象 / 根因）

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（YYYY-MM-DD，首次记录）
- **首次记录**：YYYY-MM-DD
- **优先级**：高 / 中 / 低
- **涉及模块**：如 运行时 / Dart 包 / codegen / wire / 构建
- **影响**：一句话说明影响范围（哪些功能、哪些用户受影响）

#### 问题描述

现象 + 复现方式 + 根因分析（当时的判断，写后只读）。

#### 修复记录（按时间追加，一次排查 / 修复一节）

##### 第 1 次修复（YYYY-MM-DD）

- **结果**：未修复 / 部分修复 / 已修复
- **原因**：这次为什么没修好 / 新发现（若是追加内容可省略「原因」）
- **方案**：做了什么改动（文件 / commit 可附）
- **验证**：怎么验证的（测试 / 复现步骤 / 结果）
- **遗留**：仍然存在的问题、后续待办

##### 第 2 次修复（YYYY-MM-DD）

- **结果**：未修复 / 部分修复 / 已修复
- **方案**：……
- **验证**：……
- **遗留**：……

#### 补充说明（YYYY-MM-DD）

> 只用于更正历史结论。原文不可改，在此说明原结论哪句有误、当前正确结论、依据。
```

### 1.3 模板示例

### [ID-000] 示例：某问题修复多次、结论被更正

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（2026-08-01，首次记录）
  - 已解决（2026-08-04）
- **首次记录**：2026-08-01
- **优先级**：高
- **涉及模块**：运行时
- **影响**：多 Isolate 场景下偶发崩溃

#### 问题描述

现象：后台 Isolate 关闭时偶发访问已释放内存。
复现：反复开 / 关 Isolate 可触发。
根因：初步怀疑 Session 生命周期竞态。

#### 修复记录（按时间追加）

##### 第 1 次修复（2026-08-01）

- **结果**：未修复（复现率降低但仍有）
- **方案**：Session 关闭时增加 generation 检查。
- **验证**：复现 100 次，崩溃从 20% 降到 2%。
- **遗留**：仍有极小概率崩溃，怀疑另一条路径（reply port 晚到 post）。

##### 第 2 次修复（2026-08-04）

- **结果**：已修复
- **方案**：reply port 晚到 post 按 generation 丢弃（对齐 `dispose = generation` 设计）。
- **验证**：复现 1000 次无崩溃；新增回归测试 `multi_isolate_close_test` 全绿。
- **遗留**：无。

#### 补充说明（2026-08-04）

> 更正「第 1 次修复」的根因判断：原文认为是 Session 生命周期竞态，实际根因是 reply port 晚到 post 无 generation 校验（详见第 2 次修复）。原文保留不改。

---

## 2. 已知问题与已解决记录

### 2.1 运行时 / 会话

### [ID-001] 问题标题

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（2026-08-04，首次记录）
- **首次记录**：2026-08-04
- **优先级**：中
- **涉及模块**：运行时
- **影响**：……

#### 问题描述

……

#### 修复记录（按时间追加）

（暂无）

### [ID-002] oneshot settle/start 发布顺序竞态（mpsc send 假失败、recv 假关闭、值丢失）

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（2026-08-04，首次记录）
  - 已解决（2026-08-04）
- **首次记录**：2026-08-04
- **优先级**：高
- **涉及模块**：运行时（`dart/native/include/dart_cpp_bridge/channel.hpp`，stdexec 迁移中的 `co::oneshot` / `co::mpsc`）
- **影响**：stdexec 版 channel 的所有使用者。多生产者 + 有界 mpsc 高并发下：`send()` 偶发返回 `false`（值实际已入队，误报失败）；`recv()` 虚假收到"channel closed" 提前终止；直交（pending_tx）路径的值静默丢失导致收不满。oneshot 单独使用同样可能把未写入的值当作已完成读取。

#### 问题描述

现象：

- `examples/base_demo/test_channel.cpp`（doctest）偶发失败：`mpsc bounded cross-thread completeness`（2 生产者 × 5000）与 `mpsc bounded throughput sanity`（4 生产者 × 10000）中断言 `CHECK(!prod_failed)` 失败——bounded `send()` 偶发返回 `false`，约 5-30 次 / 300 轮。
- `examples/base_demo/repro_channel.cpp`（300 轮 × 4 生产者 × 10000，容量 64）几乎每轮在 round 3-8 内复现（实测多在 round 0 即挂）：消费侧 recv 提前收到"关闭"，`got` 远小于 40000，随后 `tx.close()` 级联使其余生产者 send 失败。

根因：`oneshot::Sender::settle()` 的发布顺序错误——**先在锁外用 CAS 发布 `settled=true`，再持锁写 `status`/`value`**；而 `Receiver::opstate::start()` 持锁读 `settled`，为 true 就走"已完成"快路径直接搬出 `state_->value`。竞争窗口：settle 的 CAS 成功后、持锁前被阻塞，start() 先拿到锁 → 读到 `settled==true` 但 value 仍为空 → 以 `nullopt` 完成；settle 随后发现 `waiter==nullptr` 不再投递，写入的值随状态销毁静默丢失，且 `send()` 仍返回 true。

同一个竞态的两种表现：

- **生产侧（假失败）**：`waiting_sends` 唤醒信号的 `oneshot<bool>` 被读空 → `then(map)` 收到 `nullopt` → `value_or(false)` → `send()` 返回 `false`，但该值在被唤醒前已由 recv 推入 `bq`，并未丢失。
- **消费侧（假关闭 + 真丢值）**：`pending_tx` 直交的 `oneshot<T>` 被读空 → recv 虚假收到 `nullopt`（"channel closed"）；生产者写入直交状态的值无人接收，丢失。

误诊记录：排查期间曾在代码里加 `[dbg] send map: nullopt (signal sender dropped)` 打印，把 `nullopt` 归因于"信号 sender 被 drop"；实际是 `settled` 提前发布导致 start() 读空，与 sender 生命周期无关。

#### 修复记录（按时间追加）

##### 第 1 次修复（2026-08-04）

- **结果**：已修复
- **方案**（`channel.hpp`，对外契约零改动）：
  - `settle()` 改为在锁内先写完 `status`/`value`/`error`、最后在锁内发布 `settled`（release）；重复 settle 检测由锁外 CAS 改为锁内查 `status != kEmpty`。start() 持锁看到 `settled==true` 时 payload 必然已就绪，窗口消除。
  - `start()` 改为锁内搬出 payload、**锁外**发完成回调，消除持锁执行用户续体导致重入死锁的隐患。
  - 清理全部 `[dbg]` 调试桩（含 `oneshot<bool>` 特化打印、`destroyed` 诊断标志），以及 opstate 析构中一次无锁读 `waiter` 的 data race。
- **验证**：`dcb_repro` 修复前 round 0 必挂，修复后 2 × 300 轮全绿；`dcb_tests` 连跑 25/25 全过（含原先偶发的两个 bounded 用例）；`dcb_smoke` 回归通过。
- **遗留**：挂起的 recv opstate 若在直交投递飞行途中被销毁（中途取消），settle 取出 waiter 指针后的 deliver 仍可能触达已销毁 opstate / 丢值；析构注销已覆盖大部分情形，完整的取消语义需另行设计（现有测试不做中途取消）。

#### 补充说明（2026-08-04）

> 遗留中提到的"中途取消"问题已跟进测试：顺序取消路径（取消后才有 send 到达）确认会丢值，根因是 stale `pending_tx`，已在 [ID-003] 修复；并发 cancel-vs-settle 的 UAF 窗口仍保留为 [ID-003] 的遗留。

### [ID-003] recv 中途取消丢值（stale pending_tx）与 oneshot detached 语义

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（2026-08-04，首次记录）
  - 已解决（2026-08-04）
- **首次记录**：2026-08-04
- **优先级**：高
- **涉及模块**：运行时（`dart/native/include/dart_cpp_bridge/channel.hpp`，`co::oneshot` / `co::mpsc`）
- **影响**：挂起的 recv 被取消（opstate 中途销毁，如 Dart 侧放弃等待 / 任务 teardown）后，下一个 send 的值被静默投递给已销毁的等待者：值丢失且 `send()` 返回 `true`。bounded 与 unbounded 的直交（`pending_tx`）路径均受影响。

#### 问题描述

现象（新增取消测试，`examples/base_demo/test_channel.cpp` 8 个 `*cancel*` 用例，修复前 5 个失败）：

- `oneshot cancel parked receiver then send`：等待者取消后 `send()` 仍返回 `true`，值写入死状态后无人读取。
- `mpsc bounded/unbounded recv cancel preserves the value`、`repeated recv cancel`、`recv cancel loop`：取消的 recv 吞掉下一个（直交的）值，后续 recv 超时收不到。
- 通过用例（行为本就正确或为既定语义）：`send cancel keeps the value in the channel`、`close after recv cancel`、`concurrent stress`（修复前未触发，时序原因）。

根因（两层）：

1. oneshot 无法区分"等待者尚未 start"与"等待者曾 start 后被销毁"：opstate 析构只在锁内注销 `waiter`，状态上不留痕迹；`settle()` 看到 `waiter==nullptr` 就当"等待者稍后会来读"，照常写入并返回 `true`。
2. mpsc 的 `pending_tx` 在等待者取消后仍留在 State 里（stale）：下一个 send 抓起它直交，`settle` 不投递、值随状态销毁丢失；`bounded_send` 还直接丢弃 `direct_tx.send()` 的返回值，连失败都无从察觉。

#### 修复记录（按时间追加）

##### 第 1 次修复（2026-08-04）

- **结果**：已修复（顺序取消路径）；并发 cancel-vs-settle 的 UAF 窗口保留为遗留
- **方案**（`channel.hpp`，对外契约零改动，头文件注释中 send "detached 返回 false" 的既有承诺自此兑现）：
  - `oneshot::State` 新增 `detached`（`mu` 保护）：opstate 析构注销 waiter 时置位。
  - `settle()` 锁内检查 `detached`：已取消则失败返回；签名改为 `std::optional<T>&`，仅在成功时移动值，失败时调用方可回收。
  - `Sender` 新增 `send(std::optional<T>&)` 重载（失败时值留在参数中）与 `receiver_detached()` 查询。
  - mpsc `try_send` / `bounded_send`：抓 `pending_tx` 前先跳过 stale（`receiver_detached()` → `reset()`，走缓冲 / 挂起路径）；直交 `send` 失败（飞行途中被取消）时回收值并重走正常路径（retry 循环）。
  - 发送侧取消保持既定语义：已挂起在 `waiting_sends` 的值**不撤回**，仍进入通道（测试断言并注释为 accepted semantics）。
  - 运行时影响面核查：`cbridge.cpp` PendingOps 与 DartFn 回执的 settle 调用点均忽略 `send()` 返回值，且其 rx 侧不做中途取消，行为不变。
- **验证**：修复前 8 个取消用例 5 个失败（见问题描述）；修复后 `*cancel*` 8/8 通过（8032 断言）；`dcb_tests` 全套件连跑 25/25 全绿；`dcb_repro` 300 轮全绿；`dcb_smoke` 回归通过。
- **遗留**：`settle` 锁外 `deliver` 与 opstate 并发析构之间仍有纳秒级 UAF 窗口（`start()` 后提前销毁 opstate 本就超出 P2300 契约，靠本通道自有的注销机制支持顺序取消；并发销毁目前不保证安全）。彻底闭合需要引用计数的 waiter 句柄或等价设计；并发压力测试（3000 轮 × 2 生产者）未触发，可靠观察需 sanitizer 构建覆盖。

##### 第 2 次修复（2026-08-04）

- **结果**：已修复——协作取消路径的 UAF 窗口闭合；发送侧取消语义由「不撤回」反转为「撤回」（tokio cancel-safety）
- **方案**：按 `docs/channel_stop_token_design.md` 完成 mpsc 重构（send/recv 路径节点化 + stop token 接入）：
  - `waiting_sends` / `pending_tx` 与每次 park 的 oneshot 分配全部退役；parked 等待改为内嵌在 opstate 里的侵入式节点（send 侵入式链表 / recv 单槽指针），全部状态翻转在 `State::mu` 下裁决，完成回调一律锁外触发。
  - 值在交付（claimed）前始终属于发送方 opstate：opstate 销毁（兜底取消）或 stop 请求（协作取消）都把值撤回、不再落通道；第 1 次修复确立的「发送侧取消不撤回值」accepted semantics 自此反转，对应测试用例已改为断言撤回（`mpsc bounded send cancel withdraws the value`）。
  - send/recv opstate 通过 `stdexec::stop_token_of_t<stdexec::env_of_t<Rcvr>>` 取 token 并注册 stop 回调（`never_stop_token` 时为零成本空类型）；回调注销一律在锁外（inplace 注销会阻塞等在飞回调返回，而回调本身需要同一把 `mu`）。
  - 上文遗留的纳秒级 UAF 窗口随之闭合：协作路径下 opstate 必然活到完成信号交付之后；「认领（kClaimed）后销毁」收窄为 P2300 契约违规（与所有 stdexec 算法同一保证级别）。
  - 单消费者检查（ID-004）迁移到 recv opstate `start()`，违规者收 `set_error(logic_error)`，对外语义不变。
- **验证**：`dcb_tests` 27/27 全绿（8137 断言；新增 7 个 stop token 确定性用例：parked send/recv 取消、start 前已取消、exec::task env 透传与 set_stopped 行为验证、send/recv 取消压测守恒）；`dcb_repro` 300 轮全绿；`dcb_smoke` 回归通过；`dcb_bench` 4 生产者有界 ≈32 万 msg/s（红线 5 万）。
- **遗留**：「销毁即取消」兜底路径保留（超出 P2300 契约但受支持）；Dart 侧取消 API（task_id → stop_source 映射、`cancelTask` 式接口）不在本次范围，C++ 地基已就绪。

### [ID-004] mpsc 单消费者契约的运行期强制

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（2026-08-04，首次记录）
  - 已解决（2026-08-04）
- **首次记录**：2026-08-04
- **优先级**：中
- **涉及模块**：运行时（`dart/native/include/dart_cpp_bridge/channel.hpp`，`co::mpsc`）
- **影响**：违反单消费者约定的使用方。两个消费者并发 `recv()` 时，后一个 park 会顶掉前一个的 `pending_tx`（`oneshot::Sender` 移动赋值先 `close()` 旧状态），前一个消费者收到与正常关闭**无法区分**的假 `nullopt`（"channel closed"），属静默损坏。

#### 问题描述

tokio 用 `recv(&mut self)` 在编译期强制单消费者；C++ 没有借用检查，`Receiver` 虽 move-only，但两个线程各持 `Receiver&` 并发 `recv()` 是合法代码，类型系统挡不住，此前实现只靠命名约定，违规时静默损坏（见"影响"）。

#### 修复记录（按时间追加）

##### 第 1 次修复（2026-08-04）

- **结果**：已修复（运行期强制；编译期强制在 C++ 不可行）
- **方案**：`State::recv()` 的 park 路径检测——已有一个**活着的** parked recv（`pending_tx` engaged 且未 `detached`）→ 新的 `recv()` 直接抛 `std::logic_error`（经协程以 `set_error` 传递到调用侧），**不顶掉**旧等待者，第一个消费者不受影响；`detached`（已取消）的 stale `pending_tx` 允许覆盖，无误报。`Receiver::recv()` 注释已注明运行期强制语义。
- **验证**：新增用例 `mpsc single-consumer violation fails the offending recv`（违规者收到 `logic_error`；A 的 parked 等待不受影响，后续 send 正常投递给 A）与 `mpsc sequential recv after cancel is not a violation`（取消后顺序 recv 不误报）；`dcb_tests` 全套件 25/25、`dcb_repro` 300 轮、`dcb_smoke` 回归全绿。
- **遗留**：检测只覆盖"会顶掉 parked 等待"的违规形态；两个 `recv()` 并发但都立即取到值（无需 park）的场景不报错——通道自身仍保持一致，属应用层逻辑问题，检测它需要额外的在飞 recv 跟踪，收益不成比例。

### 2.2 构建 / 工具链

### [ID-005] hook 增量构建缓存损坏导致「莫名崩溃」（0xC0000005）

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（2026-08-05，首次记录）
  - 已解决（2026-08-05）
- **首次记录**：2026-08-05
- **优先级**：高
- **涉及模块**：构建（Native Assets build hook / MSBuild 增量）
- **影响**：foreign_runtime_demo 等走 build hook 的包，连续多次修改 header-only 模板（如 `uv_scheduler.hpp`）后，hook 增量构建产物可能 ABI 不一致，Dart 测试进程出现崩溃（`ExceptionCode=-1073741819` / 0xC0000005）。

#### 问题描述

现象：修改 `uv_scheduler.hpp` 数轮后 `dart test` 固定崩溃在 DLL 同一偏移（实测 `dcb_foreign_runtime_demo.dll+0x22fc78`），且崩溃地址多轮一致。用 `git stash` 二分回「提交版代码」后依然崩溃，一度误判为新代码引入的内存错误；排查（加日志、撤销 dispose 调用、回退 uv_work 用法）均无法消除。

根因：**hook 增量构建缓存损坏**——MSBuild 增量把旧 `.obj` 与新头文件内容混合（头文件被多个 TU 包含时部分 TU 未重编），链接产物里同一 inline/模板代码存在新旧两份定义，运行期踩到旧定义即崩溃。与源码内容无关。

#### 修复记录（按时间追加）

##### 第 1 次修复（2026-08-05）

- **结果**：已修复（缓解措施，非根治）
- **方案**：删除 `.dart_tool/hooks_runner` 与 `.dart_tool/lib` 后让 hook 全量重建。
- **验证**：清理后同一代码立即恢复全绿，`foreign_runtime_demo` 19/19 连续 3 次通过。
- **遗留**：hook 增量构建的可靠性问题未根治。后续改进方向：hook 构建前强制全量（关 MSBuild 增量 / 每次 touch 源文件时间戳），或 header-only 模板改动后手动清缓存；排查「莫名崩溃」时先清 hook 缓存再怀疑代码。

#### 补充说明（2026-08-05）

> 排查期间曾把崩溃归因于新代码（dispose 接线 / uv_work 用法），并回退了部分修复；实际根因是 hook 增量缓存损坏，回退的改动随后已恢复并提交（5f20357）。

### [ID-006] pubspec.yaml description 单行含冒号未加引号导致 YAML 解析失败

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（2026-08-05，首次记录）
  - 已解决（2026-08-05）
- **首次记录**：2026-08-05
- **优先级**：低
- **涉及模块**：Dart 包（示例 `pubspec.yaml`）
- **影响**：`dart test` / `pub get` 报 `Error on line 2, column 18: Mapping values are not allowed here`，整个包无法解析。

#### 问题描述

把多行 folded description（`description: >` + 缩进多行）改为单行时，行内 `Demo: libuv ...` 的冒号被 YAML 解析为 mapping 分隔符，报错指向冒号位置。YAML 单行标量含 `: `（冒号+空格）必须整体加引号。

#### 修复记录（按时间追加）

##### 第 1 次修复（2026-08-05）

- **结果**：已修复
- **方案**：`description` 值整体加双引号。
- **验证**：`dart test` 恢复解析与运行。
- **遗留**：无。

### [ID-007] exec::task 编译期陷阱：无默认模板参数 / 无环境下不满足 stdexec::sender 概念

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（2026-08-05，首次记录）
  - 已解决（2026-08-05）
- **首次记录**：2026-08-05
- **优先级**：中
- **涉及模块**：手写 wire / 通用启动帮助函数（`exec::task` 使用方）
- **影响**：凡直接使用 `exec::task` 的生成/手写代码，照抄文档的 `exec::task<>` 写法在 MSVC 上报「模板参数太少」；给接收 sender 的模板帮助函数加 `stdexec::sender` 概念约束时，传入 `exec::task` 编译失败。

#### 问题描述

1. `exec::task` 是 `template <class T> using task = basic_task<T, default_task_context<T>>;`（`exec/task.hpp`），**没有默认模板参数**，`exec::task<>` 不合规，必须写 `exec::task<void>`。
2. `exec::task` 的完成签名需要环境（`get_start_scheduler`）才能计算；在**无环境**的裸 `stdexec::sender` 概念检查（如 `template <stdexec::sender S> void spawn(S&&)`）下不满足概念。解法：帮助函数不约束模板参数，由 `starts_on` 等内部提供环境。

#### 修复记录（按时间追加）

##### 第 1 次修复（2026-08-05）

- **结果**：已修复
- **方案**：显式 `exec::task<void>`；`spawn_on_io` 去掉 `stdexec::sender` 概念约束（文档注释说明原因）。
- **验证**：编译通过，19/19 测试。
- **遗留**：codegen 生成器迁移到 stdexec 时，生成模板需产出 `exec::task<void>`（或等价 sender）写法。

### [ID-008] UV_HANDLE_CLOSING 是 libuv 内部宏，公共代码应使用 uv_is_closing()

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（2026-08-05，首次记录）
  - 已解决（2026-08-05）
- **首次记录**：2026-08-05
- **优先级**：低
- **涉及模块**：libuv 适配代码（`uv_worker.hpp` 的 `uv_walk` 清理）
- **影响**：用 `h->flags & UV_HANDLE_CLOSING` 判断 handle 是否在关闭流程中时，MSVC 报 C2065 未定义标识符。

#### 问题描述

`UV_HANDLE_CLOSING` 定义在 libuv 内部头（`uv-unix.h` / `uv-win.h`），公共 `uv.h` 不导出；公共 API 是 `uv_is_closing(handle)`。`uv_walk` 回调里需要跳过已在关闭流程中的 handle 时，必须用 `uv_is_closing(h)`。

#### 修复记录（按时间追加）

##### 第 1 次修复（2026-08-05）

- **结果**：已修复
- **方案**：改用 `if (uv_is_closing(h)) return;`。
- **验证**：编译通过，stop 清理路径测试通过。
- **遗留**：无。

### 2.3 其它

### [ID-009] MSVC 19.51 协程 lambda 捕获损坏（co_await 后按值捕获变量变垃圾值）

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（2026-08-05，首次记录）
  - 已解决（2026-08-05）
- **首次记录**：2026-08-05
- **优先级**：高
- **涉及模块**：codegen 输出 `wire_dispatch.cpp` / 任何 `exec::task` 协程 lambda
- **影响**：wire dispatch 的协程 lambda 按值捕获 `[session, gen, req, method]`，`co_await` 子 task 恢复后捕获变量损坏（实测 `method` 变成随机垃圾值如 3847799911），响应帧 method_id 错乱，Dart 侧匹配不到请求而超时。旧 async-simple `Lazy` 时代同样的协程 lambda 未暴露此问题，`exec::task` 协程帧布局下触发。

#### 问题描述

诊断过程：dispatch 入口打印 `method=18716410` 正确；协程 lambda 内 `post_ok` 打印 `method=3847799911`（每次运行不同值）——捕获区在协程帧内损坏。与 `src/cbridge.cpp` 既有注释「MSVC 19.51 coroutine lambda capture bug workaround: Use a static coroutine function and pass all variables as parameters」完全一致。

#### 修复记录（按时间追加）

##### 第 1 次修复（2026-08-05）

- **结果**：已修复
- **方案**：`wire_dispatch.cpp` 全部异步 case 改为**静态协程函数**，参数显式传递（`spawn_on_io(静态函数(session, gen, req, method, ...))`）。
- **验证**：修复后测试 1 立即通过，全套 19/19。
- **遗留**：codegen 生成器仍产出协程 lambda，重新生成会退回坏代码；生成器迁移到 stdexec 时（AGENTS.md 规则 4）必须同时改为静态函数或避免捕获。

### [ID-010] 协程内持有全局锁跨 co_await 阻塞 io 线程（并发请求死锁）

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（2026-08-05，首次记录）
  - 已解决（2026-08-05）
- **首次记录**：2026-08-05
- **优先级**：高
- **涉及模块**：业务协程（`foreign_api.cpp` 的 `ask_uv` 等）
- **影响**：并发调用 `ask_uv` / `uv_compute` 等持有全局锁 `g_mu` 跨 `co_await` 的函数时，io 线程被第二个请求的锁等待阻塞，第一个请求的完成（需要 io 线程恢复）永远无法处理，永久死锁（Dart 侧超时 / `resource deadlock would occur`）。

#### 问题描述

规则：**io 线程必须保持空闲**才能恢复挂起的协程（exec::task 的完成经 `asio::post` 回 home scheduler）。锁跨 `co_await` 持有 = io 线程可能被别的协程阻塞在锁上 = 自死锁。锁只能用于获取 scheduler / 启动任务，挂起前必须释放。

#### 修复记录（按时间追加）

##### 第 1 次修复（2026-08-05）

- **结果**：已修复
- **方案**：所有协程函数的锁作用域收窄到 `require_worker()`（取 scheduler）为止，`co_await` 前释放；`test_channel_service` 系列同理（锁内只做建通道 + start_detached）。
- **验证**：`multiple concurrent requests` 测试通过；全套 19/19。
- **遗留**：无。

### [ID-011] uv_scheduler 并发设计陷阱（timer double-close / cancel 挂起 / WorkState 双重管理）

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（2026-08-05，首次记录）
  - 已解决（2026-08-05）
- **首次记录**：2026-08-05
- **优先级**：高
- **涉及模块**：`examples/foreign_runtime_demo/uv_scheduler.hpp`（UvScheduler / schedule_after / uv_work）
- **影响**：libuv 适配的并发正确性——若按初版实现发布，cancel/init 竞态可致 `uv_close` 二次调用（libuv assert / 损坏 closing 队列）、stop 提前到达时 receiver 永不完成（挂起）、`uv_work` 双重释放（use-after-free）。两轮 review 发现并修复。

#### 问题描述

三个并发缺陷：

1. **timer double-close**：cancel claim 先赢、`TimerInitNode` 后执行时，init 兜底已 `uv_close`（claim != kPending 分支），随后 `TimerCancelNode` 再 `uv_timer_stop + uv_close` → 对 closing handle 二次 close。
2. **receiver 挂起**：stop 请求在 init 节点执行前到达时，cancel 节点的 `!inited` 分支直接 return，init 兜底只 close 不完成 → 接收者永不完成。
3. **WorkState 双重管理**：`make_shared` 创建却在 `after_work` 里 `delete ws`，opstate 析构还会访问 `ws_->claim` → use-after-free + 控制块双重释放；且堆分配节点（TimerInitNode/TimerCancelNode/WorkStartNode）无释放机制，每操作泄漏。

#### 修复记录（按时间追加）

##### 第 1 次修复（2026-08-05）

- **结果**：已修复
- **方案**：
  - `TimerState` 增加 `std::atomic<bool> closed`：init 兜底 / `on_timer` / cancel 节点中**恰好一个**执行 close（`closed.exchange(true)` 仲裁），double-close 消除。
  - `TimerCancelNode` 的 `!inited` 分支补 `set_stopped`（`complete_stopped_` 且 receiver 未移走时），receiver 不再挂起。
  - `WorkState` 改为 shared_ptr 自引用（`self`，`WorkStartNode::run` 在 `uv_queue_work` 前设置、`after_work` 释放），支持 void 返回值（`stored_t`），`uv_queue_work` 失败时 `set_error` 完成。
  - `StartNode` 增加虚拟 `dispose()`：`on_async` 在 `run()` 后调用，堆节点 `delete this`，嵌入式 schedule opstate 空覆写；堆节点泄漏消除。
  - `UvWorker::stop()` 用 `uv_walk` + `uv_is_closing` 清理残留 handle（含未完成 timer），`uv_loop_close` 不再因活跃 handle 失败。
- **验证**：19/19 测试连续多次通过（含 `uv_compute` 经 `uv_work` 真实执行线程池路径）；stop/restart 测试覆盖清理路径。
- **遗留**：① stop 时在飞的 `uv_work` 无法取消（`after_work` 需要 loop 运行，loop 已停则 receiver 挂起 + 自引用泄漏）——demo 测试无此路径，已文档化；② `uv_queue_work` 失败且 opstate 并发析构时 `self` 未 reset 的罕见泄漏（LOW，review 记录）；③ schedule opstate destroy-vs-run 窗口为 P2300 拥有者契约场景（与 `co::oneshot` 相同保证级别）。

##### 第 2 次修复（2026-08-05）

- **结果**：已修复（遗留 ① ② ③ 全部解决，见 ID-014 / ID-015）
- **方案**：
  - 遗留 ①（stop 时在飞 `uv_work`）：`UvSchedState` 增加 `work_in_flight` 计数（push 时在锁内递增，`after_work` / `run()` 失败分支 / `on_discarded()` 配对递减）；`UvWorker::stop()` 在 `uv_stop` 前轮询等待「start 队列空 && 计数归零」，保证 `after_work` 一定在 loop 停止前执行；`uv_loop_close` 失败会打日志。
  - 遗留 ②（`uv_queue_work` 失败 + 已取消时 `self` 未 reset）：`WorkStartNode::run()` 失败分支在 CAS 失败路径也执行 `ws_->self.reset()`。
  - 遗留 ③（destroy-vs-run 窗口）：见 ID-014（claim 基类化 + 锁内抢占）。
- **验证**：19/19 测试连续多次通过；review + security_review 续审通过。
- **遗留**：无（stop 轮询无超时——当前计数配对可证终止，属防御性改进项）。

### [ID-012] dispose() UAF——on_async 在 run() 后触碰已析构的嵌入式 opstate（0xC0000005）

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（2026-08-05，首次记录）
  - 已解决（2026-08-05）
- **首次记录**：2026-08-05
- **优先级**：高
- **涉及模块**：`examples/foreign_runtime_demo/native/uv_scheduler.hpp`（start 队列 / `on_async` / `StartNode`）
- **影响**：ask_uv 测试间歇崩溃（访问违例 0xC0000005），崩溃点经 dumpbin 反汇编定位为 `on_async` 内的第二次虚调用（`call [rax+8]`）；多线程调度下稳定复现，曾误判为构建缓存问题（见 ID-013）。

#### 问题描述

初版生命周期设计：`on_async` 在 `run()` 返回后调用 `start->dispose()` 虚函数（堆节点 `delete this`，嵌入式 schedule opstate 空覆写）。

竞态：`run()` 内 `set_value` 完成后，io 线程可立即析构 `starts_on` 链（含嵌入式 schedule opstate）——P2300 契约允许完成即析构。loop 线程在 `run()` 返回后调用 `dispose()` 时读取已释放对象的 vtable → UAF。loop 线程在 `run()` 返回与 `dispose()` 之间被抢占时窗口显著扩大（io 线程可完整跑完 post_ok 并析构整棵树）。

修复前崩溃栈（RtlCaptureStackBackTrace + dumpbin RVA 对照）：`on_async` → `call [rax+8]`（dispose 虚调用），调用者链 `uv__process_async_wakeup_req` → `uv__process_reqs` → `uv_run` → `UvWorker::start` lambda。

#### 修复记录（按时间追加）

##### 第 1 次修复（2026-08-05）

- **结果**：已修复
- **方案**：移除 `dispose()` 机制——堆节点（`TimerInitNode` / `TimerCancelNode` / `WorkStartNode`）在 `run()` 各返回路径末尾 `delete this`；`on_async` 在 `run()` 返回后不再触碰节点（只使用预先保存的 `next` 指针）。嵌入式 schedule opstate 由父链析构。
- **验证**：修复后 ask_uv 不再崩溃；19/19 连续多次通过（含清缓存全量重建，确认 DLL 为最新代码）。
- **遗留**：无（本条修复后即闭环；pop 窗口竞态见 ID-014）。

### [ID-013] hook 依赖监控缺失——native/ 外的头文件修改不触发重建

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（2026-08-05，首次记录）
  - 已解决（2026-08-05）
- **首次记录**：2026-08-05
- **优先级**：高
- **涉及模块**：`dart/lib/hook.dart`（`DcbCMakeBuilder._declareDependencies`）/ foreign_runtime_demo 目录布局
- **影响**：`examples/foreign_runtime_demo/` 根目录下的 `uv_scheduler.hpp` / `uv_worker.hpp` 不在 hook 的依赖跟踪范围（只遍历 `sourceDir` = `native/`），修改它们后 `dart test` 的 hook 缓存不失效、**一直编译并运行旧 DLL**——表现为「改了代码没效果」「间歇崩溃无法复现」，并直接导致 ID-005（hook 缓存损坏误诊）与 ID-012（崩溃定位困难）的排查走弯路。

#### 问题描述

`DcbCMakeBuilder._declareDependencies` 递归列出 `sourceRoot`（`sourceDir` 参数，foreign_runtime_demo 里为 `native/`）下的 `.h/.hpp/.cpp/...` 文件并声明为 code_assets 缓存依赖。头文件若放在 `native/` 之外（通过 `target_include_directories(... ${CMAKE_CURRENT_SOURCE_DIR}/..)` 引入），修改它们不会改变 hook 输入 hash → hooks runner 跳过 hook → 不重新 cmake 配置 / 编译。

判定方法：对比 `.dart_tool/lib/dcb_foreign_runtime_demo.dll` 与头文件的 mtime；多次「修改 → 崩溃地址不变」的强信号。

#### 修复记录（按时间追加）

##### 第 1 次修复（2026-08-05）

- **结果**：已修复
- **方案**：把 `uv_scheduler.hpp`、`uv_worker.hpp` 移入 `native/`（include 路径同步调整），使其纳入 hook 依赖跟踪；此后修改头文件即可触发重建。
- **验证**：修改头文件后 DLL mtime 更新；19/19 连续多次通过。
- **遗留**：无（若未来在 `native/` 外放头文件，需为 `DcbCMakeBuilder` 增加额外依赖目录参数）。

### [ID-014] start 队列 pop 窗口竞态与 unlink tail-orphan（claim 基类化修复）

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（2026-08-05，首次记录）
  - 已解决（2026-08-05）
- **首次记录**：2026-08-05
- **优先级**：高
- **涉及模块**：`examples/foreign_runtime_demo/native/uv_scheduler.hpp`（`StartNode` / `on_async` / schedule opstate 析构）
- **影响**：① 析构（锁内 CAS `kQueued→kCancelled`）可在 `on_async` 的 `pop_all()` 之后、`run()` 之前成功——节点已出队无法 unlink，析构销毁 receiver 后 `run()` 仍会执行 → UAF；② 析构 unlink 摘除 tail 节点时把 `tail` 置 `nullptr`（即使队列还有其他节点），下一次 `push()` 会覆盖 `head` → 剩余节点全部孤儿，其 sender 永不完成。

#### 问题描述

1. **pop 窗口**：`on_async` 在锁内 `pop_all()` 后释放锁，再在锁外 `run()`。析构也在锁内做 `kQueued→kCancelled` CAS 与 unlink——若析构发生在 pop 之后（节点已不在队列，unlink 无效果），CAS 成功即销毁 receiver 与节点内存，`run()` 随后访问已析构对象。security_review 判定为 MEDIUM（当前调用图不可达，但 `when_all` 取消 / 整树析构可触发）。
2. **tail-orphan**：析构 unlink 用 `Node** pp` 遍历，摘除时若 `st_->tail == this` 则 `st_->tail = nullptr`；但当被摘除节点不是 head 时，队列仍有存活节点，`tail=nullptr` 后 `push()` 走 `else head = n` 分支 → 覆盖 head，原存活节点失去引用。

#### 修复记录（按时间追加）

##### 第 1 次修复（2026-08-05）

- **结果**：已修复
- **方案**：
  - `claim` 移到 `StartNode` 基类（`enum Claim` + `std::atomic<int>`）。
  - `on_async` 在 `pop_all()` 的**同一锁临界区内**对每个节点 CAS `kQueued→kRunning`（与析构的 `kQueued→kCancelled` 互斥）；失手节点收集到 dead 链表，锁外经 `on_discarded()` 释放（堆节点 `delete this`，嵌入式 opstate 空覆写，`WorkStartNode` 顺带撤销 push 时计数）。
  - schedule opstate 析构改 prev 指针遍历 unlink：`tail = prev`（仅当被摘除节点是 head 时 `tail=nullptr`）；receiver 改为裸指针，析构只在取消路径（锁外）`delete`，`run()` 先取局部再 `set_value` 后 `delete`，closed 路径先移出局部再 `set_stopped`（防完成链重入销毁树后写已销毁对象）。
- **验证**：19/19 连续多次通过；review 续审确认 tail-orphan 与计数配对全路径正确。
- **遗留**：已抢占未 run 的节点被整树销毁仍是 P2300 契约外场景（本代码库所有树为 `start_detached` 所有，不可达），已在 `schedule()` 注释文档化。

### [ID-015] teardown TOCTOU 与 stop() 时挂起 timer 泄漏

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（2026-08-05，首次记录）
  - 已解决（2026-08-05）
- **首次记录**：2026-08-05
- **优先级**：中
- **涉及模块**：`examples/foreign_runtime_demo/native/uv_scheduler.hpp`（`work_in_flight` / `TimerState::on_close`）+ `native/uv_worker.hpp`（`UvWorker::stop`）
- **影响**：① `stop()` 与 start 队列 drain 的 TOCTOU：`work_in_flight` 在 `WorkStartNode::run()` 内（`uv_queue_work` 成功后）递增，`stop()` 可能在 `on_async` pop 批量后、run 之前观察到「队列空 + 计数 0」→ `uv_stop`/`join` → 节点仍执行 `uv_queue_work`（loop 在回调中）成功，但 `after_work` 永不运行 → 计数卡 1、`WorkState` 自引用泄漏、`uv_loop_close` 失败；② `stop()` 的 `uv_walk` 关闭仍挂起的 `schedule_after` handle 时只释放 `TimerState` 自引用，receiver 永不完成 → 拥有者 sender 树（opstate + exec::task 帧）泄漏。

#### 问题描述

1. **TOCTOU**：计数递增时机在 `run()`（loop 线程）而停止判定（`stop()`，调用方线程）读「队列空 + 计数 0」——两者之间存在窗口。修复方向（security_review 建议）：push 时在锁内递增。
2. **timer 泄漏**：`TimerState::on_close` 原实现只 `self.reset()`；`stop()` 的 teardown 路径（`uv_walk` + 第二次 `uv_run`）关闭仍为 `kPending` 的 timer 时，无任何完成者 → 树泄漏。复现场景：stream 订阅中途取消或未等待 timer 完成直接 `stop_uv_worker`（Dart 测试通过先 await 完成规避）。

#### 修复记录（按时间追加）

##### 第 1 次修复（2026-08-05）

- **结果**：已修复
- **方案**：
  - `work_in_flight` 改为 push 时（锁内）递增；`after_work`（最先执行）、`WorkStartNode::run()` 失败分支、`on_discarded()` 三处配对递减；`stop()` 在 `uv_stop` 前轮询等待「队列空 && 计数 0」；`uv_loop_close` 失败打日志。
  - `TimerState::on_close` 增加 CAS `kPending→kStopped`：成功（即 teardown 关闭未决 timer 路径）则把 receiver 移到局部、`self.reset()`、`set_stopped` 完成树；失败（正常超时 / 取消已认领）只 `self.reset()`。完成点在 loop 线程的第二次 `uv_run` 内，先于 `uv_loop_close` / `st_.reset()`。
- **验证**：19/19 连续多次通过；security_review 续审 verdict=pass（claim 仲裁单一完成者、顺序无 UAF、teardown 顺序正确）。
- **遗留**：stop 轮询无超时（当前计数配对可证终止，若未来漏减会从泄漏变成挂起——建议加有界等待兜底）；无 stop-with-pending-timer 的专门回归测试（现由 uv_stream / stop_uv_worker 路径间接覆盖）。

### [ID-016] stdexec FetchContent 的 execution.bs 下载失败留 0 字节文件 → CMake `VERSION "0..0"`

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（2026-08-05，首次记录）
  - 已解决（2026-08-05，环境级处理）
- **首次记录**：2026-08-05
- **优先级**：中
- **涉及模块**：构建 / 工具链（`dart/native/CMakeLists.txt` 的 stdexec `FetchContent_Declare`；hook 全量重建路径）
- **影响**：清空 hook 缓存全量重建时，stdexec 配置阶段报 `string sub-command REGEX, mode REPLACE needs at least 6 arguments` + `project(...) VERSION "0..0" format invalid`，hook 构建失败，容易被误判为代码或 CMake 配置问题。

#### 问题描述

`dart/native/CMakeLists.txt` 用 `FetchContent` 拉取 stdexec `nvhp-26.05` 标签。该版本的 `CMakeLists.txt` 从 `raw.githubusercontent.com/cplusplus/sender-receiver/main/execution.bs` 下载规范文件并提取 `Revision:` 作为版本号：

```cmake
if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/execution.bs)
  file(DOWNLOAD ".../execution.bs" ${CMAKE_CURRENT_BINARY_DIR}/execution.bs)
endif()
file(STRINGS ".../execution.bs" STD_EXECUTION_BS_REVISION_LINE REGEX "Revision: [0-9]+")
...
project(STDEXEC VERSION "0.${STD_EXECUTION_BS_REVISION}.0")
```

`file(DOWNLOAD)` 网络失败时**静默留下 0 字节文件**（不报错）；下次配置因 `NOT EXISTS` 为假跳过下载 → `STD_EXECUTION_BS_REVISION` 为空 → `VERSION "0..0"`。

#### 修复记录（按时间追加）

##### 第 1 次修复（2026-08-05）

- **结果**：已修复（环境级；代码无需改动）
- **方案**：用 curl 下载有效的 `execution.bs`（约 432 KB，`Revision: 11`）覆盖 `stdexec-build/execution.bs` 后重试 hook 构建；网络恢复时全量重建自然成功。
- **验证**：覆盖后 hook 配置与构建成功；19/19 测试通过。
- **遗留**：`file(DOWNLOAD)` 失败不留痕迹属上游 stdexec 行为；离线 / 弱网环境首次全量构建仍可能踩中。可选改进：vendored 版本（`third_party/stdexec`，固定 `VERSION "0.11.0"`，无此下载逻辑）替换 FetchContent。

### [ID-017] 生成 wire 的 spawn_on_io 模板链在 MSVC 上峰值 >150 GB

- **状态记录**（只追加，最新一行即当前状态）：
  - 已解决（2026-08-06，spawn_on_io 改为非模板 + 参数固定为 `exec::task<void>`）
- **首次记录**：2026-08-06
- **优先级**：高（编译直接 OOM）
- **涉及模块**：`dcb_gen_tool/scripts/generate.py` 生成的 `wire_dispatch.cpp` / `wire_slice.cpp`
- **影响**：生成的 dispatch 文件里，若 `spawn_on_io` 写成模板（按 dispatch 函数类型实例化 `starts_on/upon_error/upon_stopped/start_detached` 链），每个 dispatch 函数各实例化一整条链；几十个 dispatch 的模板实例化让 MSVC 峰值超过 150 GB（40 GB 沙盒直接 OOM / 本机换页卡死）。

#### 问题描述与修复

- **根因**：所有 dispatch 函数都返回 `exec::task<void>`，但模板版 `spawn_on_io(S&&)` 会按**每个调用点的 S 类型**（每个 dispatch 协程的 promise/状态机类型）重新实例化 `starts_on | upon_error | upon_stopped | start_detached` 整条链（约 131 个模板实例化 × 46 个 dispatch）。
- **修复**：`spawn_on_io` 改为普通函数，参数固定为 `exec::task<void>`（按值）；所有 dispatch 协程的 task 都转换为该固定类型，整条链只实例化一次。`exec::task<void>` 的协程状态机类型一致，所以这是类型安全的。
- **验证**：修复后 `wire_dispatch.cpp`（46 个 dispatch）Debug/Release 编译峰值约 1.0–1.3 GB，编译成功。
- **遗留**：无（本条闭环）。

#### 补充说明（2026-08-06）

> 「遗留：无」之后继续演进：`spawn_on_io` 已改回**模板 + 泛型约束**形式（对齐 `examples/foreign_runtime_demo/native/generated/wire_dispatch.cpp` 参考实现），但 OOM **不会复发**，原因如下：
> - 新签名：`template <class S> requires stdexec::sender_in<S, spawn_env_t> void spawn_on_io(S&& sndr)`。`spawn_env_t` 模拟 `starts_on(io_scheduler, sndr)` 实际提供给子 sender 的环境（`get_scheduler` / `get_start_scheduler` 均答 `dcb::IoContextScheduler`）；用 `sender_in<S, spawn_env_t>` 而非 `stdexec::sender`，是因为 `exec::task` 的 completion signatures 需要在该环境下计算（plain `sender` 概念会拒绝 `exec::task`）。
> - 原文根因「按每个调用点的 S 类型实例化」在**当前生成代码中不再成立**：所有 dispatch 函数统一声明返回 `exec::task<void>`，调用点表达式类型一致 → S 推导为同一类型 → 整条链仍只实例化一次（编译实测约 1 GB 峰值、无 OOM）。当年 131 × 46 个实例化属于 async-simple `Lazy` 时代（每个 dispatch 返回不同的具体协程类型）。
> - 额外收益：约束在调用点给出清晰编译错误（误传非 sender 时），注释已同步更新（不再引用 OOM 排查史）。

### [ID-018] exec::task 内 co_await DartFn（dartfn_sender）触发 continues_on 包装 → MSVC 错误风暴 + ~140 GB 提交内存

- **状态记录**（只追加，最新一行即当前状态）：
  - 已解决（2026-08-06，`dart_fn.hpp` 的 `dartfn_sender` 新增 `dartfn_env` 报告 `__asynchronous_affine`）
- **首次记录**：2026-08-06
- **优先级**：高（编译直接 OOM / 卡死）
- **涉及模块**：`dart/native/include/dart_cpp_bridge/dart_fn.hpp`；业务代码（`api_impl/bridge_api.cpp`、`api_impl/counter.cpp`）里的 `co_await callback(...)`
- **影响**：在 `exec::task` 协程里 `co_await` 一个 `DartFn`（如 `auto reply = co_await callback(name);`），MSVC 14.51 编译该翻译单元时（1）报 `stdexec/__variant.hpp(339): error C2338: static assertion failed: 'Type not in variant'`；（2）错误恢复阶段提交内存峰值约 140 GB（任务管理器显示 ~150 GB），40 GB 沙盒 OOM、本机 64 GB 疯狂换页卡死。

#### 问题描述

- **机制**：`DartFn::operator()` 返回 `dcb::detail::dartfn_sender<Ret>`（stdexec sender，内部 base 链为 `oneshot::Receiver | continues_on(io_scheduler) | then(decode)`）。`exec::task` 的 `promise::await_transform(sender)` 会检查 `__completes_where_it_starts<set_value_t, env_of_t<Sender>, promise_env>`：
  - `dartfn_sender::get_env()` 原来返回 `base_.get_env()`（透传 `continues_on` 的 env，其 `__get_completion_behavior` 查询一路透传到底层 `oneshot::Receiver`，而 channel 的 receiver sender 没有该属性 → `__unknown` → 非 affine）。
  - 非 affine → `await_transform` 把 sender 再包一层 `continues_on(get_start_scheduler(ctx), sndr)`。task 的 sticky 调度器是类型擦除的 `__any_scheduler`，于是实例化 `continues_on(__any_scheduler, schedule_from(...))` 链：
    - `schedule_from` 需要把 completion 存进 `__variant`（value/error/stopped 三路），而 `dartfn_sender` 的 `completion_signatures` 只有 value/error 两路 → `__variant.hpp(339)` 静态断言失败；
    - 断言失败后 MSVC 进入模板错误恢复，反复展开巨型模板签名（几十 KB/条 × 上百条错误）→ 提交内存爆炸到 ~140 GB。

#### 修复（2026-08-06）

- **方案**：`dartfn_sender` 新增自定义 env `dcb::detail::dartfn_env`，对 `stdexec::__get_completion_behavior_t<_Tag>` 查询返回 `__completion_behavior::__asynchronous_affine`（语义正确：base 链内建 `continues_on(io_scheduler)`，完成固定发生在 io 线程）；`get_env()` 改为返回 `dartfn_env{}`。
- **效果（静态推演）**：`__completes_where_it_starts` 为 true → `exec::task::await_transform` 走 if 分支，**不再包 `continues_on`**；`as_awaitable` 落到 `__with_sender` → `__sender_awaiter`（identity 适配，直接 connect base 链）→ 无 `__any_scheduler` / `schedule_from` 组合 → 'Type not in variant' 消失，实例化量级与 wire_dispatch.cpp 相当（~1 GB）。
- **兼容性**：
  - `sync_wait(callback(...))` 等 sender 管道用法不受影响（connect 路径不依赖该 env 查询）；
  - `starts_on(sched, dartfn)` 会因 affine 走 `get_completion_scheduler` 快速路径，运行语义不变（base 链决定实际完成线程）；
  - 两个 stdexec 版本（FetchContent 下载版 `nvhp-26.05` 与 vendored `third_party/stdexec`）都有 `__get_completion_behavior_t` / `__completion_behavior` 内部 API。
- **验证状态**：⚠️ 静态分析完成，**尚未编译验证**（本机不再裸跑 cl.exe 以防卡死；请在 40 GB 沙盒 / 有内存上限的环境验证）：逐个编译 `api_impl/bridge_api.cpp`、`api_impl/counter.cpp`（两者含 `co_await DartFn`，修复前错误风暴 + OOM），再编 `wire_dispatch.cpp` 与其余 api_impl 文件。
- **遗留**：`foreign_api.cpp` / `multi_runtime_api.cpp` 里还有大量 `co_await` 裸 sender（`oneshot::Receiver`、`mpsc recv`、`dcb::sleep/async_wait`）。它们同样会走 `continues_on(__any_scheduler, ...)` 链，但 completion 签名含 set_stopped 且 variant 组合合法（无静态断言失败），历史上未报告 OOM；若沙盒里仍超限，可对同类 sender 套用同样的 affine env 方案（按各自实际完成上下文报告）。

#### 编译验证（2026-08-06）

- **结果**：已编译验证通过（本条闭环）
- **验证**：codegen_demo 全量 hook 构建（MSVC 14.51，Debug）成功——`api_impl/bridge_api.cpp`、`api_impl/counter.cpp`、`wire_dispatch.cpp` 及全部 api_impl / 生成文件编译无错误风暴、无 OOM；`flutter test integration_test` 149/149 通过，其中覆盖 `co_await DartFn` 运行路径的有：`greet_dart_fn`、`opaque class Counter greetDartFn`、`DartFn from worker runtime call Dart callback from Worker A/B`、`10 concurrent DartFn from Worker A`、`10 concurrent DartFn split across A and B` 等。
- **遗留**：无（「遗留」中提到的裸 sender 组合未触发 OOM，保持现状即可）。

### [ID-019] stdexec `inplace_stop_source::request_stop()` 返回值语义与 `std::stop_source` 相反（cancel_task 契约回归）

- **状态记录**（只追加，最新一行即当前状态）：
  - 已解决（2026-08-06，codegen_demo 集成测试 149/149 通过）
- **首次记录**：2026-08-06
- **优先级**：高
- **涉及模块**：`examples/codegen_demo/native/api_impl/bridge_api.cpp`（stdexec 迁移，C01-C06 取消测试）
- **影响**：Dart `cancelTask` 的返回值与既有契约相反——调用方按 `std::stop_source` 语义判断"首次取消应返回 true"会误判为取消失败；实际取消功能正常（任务确实被取消），但 C02/C03/C04/C06 集成测试断言失败，且失败在 flutter test 里被渲染成难读的 stack_frame assertion。

#### 问题描述

迁移前（async-simple）`cancel_task` 返回 `signal->emits(SignalType::Terminate) != SignalType::None`：`emits` 返回**之前**的状态，因此 **true = 本次调用发起了取消**（标准语义）。迁移到 stdexec 后直接透传：

```cpp
return stop_source->request_stop();
```

而 stdexec 的 `inplace_stop_source::request_stop()`（`third_party/stdexec/include/stdexec/stop_token.hpp:263-297`）返回值语义**相反**：`__try_lock_unless_stop_requested_` 失败（已停止过）返回 `true`；本次调用首次发起 stop 返回 `false`。

现象（`flutter test integration_test` C02 等 4 个用例失败）：

- `cancelTask(taskId)` 返回 `false`，测试断言 `isTrue` 失败；
- 任务**确实**被取消了：future 以 `StateError: ... task cancelled by signal: ...` 失败；
- 由于 future 在 `_waitUntil` 轮询期间（`await expectLater(...)` 之前）就已失败，flutter test 报 unhandled async error；渲染错误栈时又触发 `flutter/src/foundation/stack_frame.dart:197` 的 assertion（`FlutterError.demangleStackTrace` 未设置的已知问题），真实错误被吞掉，只剩一段无头绪的框架栈。

排查手段：临时调试测试（对 future 立即挂 `then(onError)` 打印）确认 `cancelResult=false` + 任务确实被取消，从而锁定是返回值语义问题而非取消未生效。

#### 修复记录（按时间追加）

##### 第 1 次修复（2026-08-06）

- **结果**：已修复
- **方案**（`bridge_api.cpp`）：反转返回值并注释语义差异：

  ```cpp
  // stdexec::inplace_stop_source::request_stop() has the OPPOSITE return
  // semantics ... Invert it so the Dart API keeps the established contract:
  // true = this call cancelled the task.
  return !stop_source->request_stop();
  ```

  未改动生成代码：Dart 侧 `cancelTask` 纯透传 `bool`，契约在 C++ 实现 + 测试断言中（C01: 二次取消返回 false；C02/C03/C04: 首次取消返回 true；C05: 未知 id 返回 false）。
- **验证**：修复后 `flutter test integration_test` 149/149 通过（C01-C06 全绿）。
- **遗留**：无。同类陷阱提醒：迁移 async-simple `Signal::emits()` / 其它返回"之前状态"的 API 时，需逐一核对新 API 的返回值语义。

### [ID-020] stdexec 迁移中 cbridge DartFn 回调契约回归（错误改抛异常 / 丢失 "C:" 前缀）

- **状态记录**（只追加，最新一行即当前状态）：
  - 已解决（2026-08-06，codegen_demo 集成测试 149/149 通过）
- **首次记录**：2026-08-06
- **优先级**：中
- **涉及模块**：`examples/codegen_demo/native/api_impl/foreign_api.cpp`（`test_cbridge_invoke` / `on_dart_reply_pure_c`，cbridge pure C API 测试组）
- **影响**：3 个 cbridge 集成测试失败：`dcb_invoke_dart_fn: Dart callback that throws`（期望返回 `ERROR:` 前缀字符串）、`pure C invoke: dcb_async_create + C callback + async_wait` 与 `pure C invoke: Dart callback with delay`（期望返回 `C:` 前缀字符串）。测试文件未改动，即这些是迁移引入的行为回归而非测试变更。

#### 问题描述

`test_cbridge_invoke`（DartFn 经 `dcb_invoke_dart_fn` 纯 C API 回调）：

- 迁移前：C 回调里 Dart 抛异常时 `p->set_value(std::string("ERROR:") + error)`，错误以**正常返回值**字符串形式回到协程（`async_wait` 正常完成，`ByteReader::str()` 解出 "ERROR:..."）。
- 迁移后：改成 `dcb_async_fail(c->op, error)` → `async_wait` 抛 `std::runtime_error` → wire 层 `post_err` → Dart 侧 `await` 抛 `StateError`。测试契约（`expect(result, startsWith('ERROR:'))`）被破坏。

`on_dart_reply_pure_c`（pure-C 路径回调）：

- 迁移前：成功路径**解码** Dart 返回值（`dcb_read_str`）、前置 `"C:"` 前缀（`snprintf`）、重新编码后 `dcb_async_complete`——演示"C 层加工回复"的完整流程。
- 迁移后：被简化成原样透传 `dcb_async_complete(ctx->op_id, data, len)`，丢失前缀。测试期望 `C:dart-pure:hello` / `C:delayed:wait` 失败。

#### 修复记录（按时间追加）

##### 第 1 次修复（2026-08-06）

- **结果**：已修复
- **方案**（`foreign_api.cpp`，两处均恢复旧行为）：
  - `test_cbridge_invoke` 的 C 回调：错误分支改为构造 `"ERROR:" + error` 字符串、经 `dcb_writer` 编码后 `dcb_async_complete` 正常完成（不再 `dcb_async_fail`）；纯 C 路径 `on_dart_reply_pure_c` 的错误分支保持 `dcb_async_fail`（对应测试 `pure C invoke: Dart callback that throws` 期望抛异常，两路径契约不同，不可互相套用）。
  - `on_dart_reply_pure_c`：恢复 `dcb_reader` 解码 + `snprintf("C:%.*s")` 前缀 + `dcb_writer` 重新编码 + `dcb_async_complete`。
- **验证**：修复后 `flutter test integration_test` 149/149 通过（cbridge pure C API 组 12 个用例全绿）。
- **遗留**：无。安全性说明：`dcb_async_complete`（`cbridge.cpp:145`）内部 `r.data.assign(...)` 同步复制数据，回调内 `dcb_writer_free` 无 use-after-free；`snprintf` 以 `slen` 为精度、buffer 512 字节截断时仍 NUL 结尾，`dcb_write_str` 无越界。

### [ID-021] codegen_demo 全量集成测试偶发挂起（libuv multiple concurrent requests 超时 5 分钟）

- **状态记录**（只追加，最新一行即当前状态）：
  - 未解决（2026-08-06，偶发，未能稳定复现）
- **首次记录**：2026-08-06
- **优先级**：低
- **涉及模块**：`examples/codegen_demo` 的 uv 相关测试（`libuv foreign runtime` 组的 `multiple concurrent requests`，4 个并发 `askUv`）与全量 `flutter test integration_test` 执行顺序
- **影响**：全量测试偶发在该用例挂起约 5 分钟（integration_test 默认超时），随后所有剩余测试（含 setUpAll/tearDownAll）报 `did not complete`、进程以失败退出。同一份二进制后续 libuv 组单跑 15/15、全量重跑 149/149 均通过——不影响正常交付，但会浪费一次全量 CI 时间并造成误报。

#### 问题描述

现象（2026-08-06 观察一次，未能复现）：

- 全量跑在 `+85`（`multiple concurrent requests`）卡住，从 00:01 到 05:25 无输出，随后全部剩余测试报 `did not complete`，`exit status 1`。
- 卡住时计数停在 `+85` 不变 → 是第 85 个测试本身未完成，而非后续测试级联失败。
- 同一份代码立即重跑：`--plain-name "libuv"` 单跑 15/15 通过（00:00）；全量重跑 149/149 通过（00:04）。

根因推测（未证实）：`askUv` 应答链（uv 线程 `send` → oneshot channel → io 线程协程恢复 → `post_ok`）中某个环节偶发丢失响应（uv_scheduler 并发区域历史雷区 ID-011~015 的残余窗口），导致 `Future.wait` 永不完成。**与 spawn_on_io 模板化改动无关**：改动前后各跑多次，仅此一次挂起，且挂起点前后行为等价。

#### 修复记录（按时间追加）

（暂无——偶发未复现，未定位根因。若再次复现：优先抓 `multiple concurrent requests` 挂起时的 Dart 侧调用栈与 `pending_ops` 残留，检查 uv worker 的 start 队列与 oneshot channel 是否丢应答。）
