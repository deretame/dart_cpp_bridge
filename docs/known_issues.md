# dart_cpp_bridge 已知问题与技术债

> 更新日期：2026-08-05（新增 ID-005 至 ID-011：foreign_runtime_demo stdexec 迁移（libuv UvScheduler）过程中碰到的构建 / 协程 / 并发陷阱）
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
  - [2.3 其它](#23-其它)
    - [2.3.1 [ID-009] MSVC 19.51 协程 lambda 捕获损坏（co_await 后按值捕获变量变垃圾值）](#231-id-009-msvc-1951-协程-lambda-捕获损坏co_await-后按值捕获变量变垃圾值)
    - [2.3.2 [ID-010] 协程内持有全局锁跨 co_await 阻塞 io 线程（并发请求死锁）](#232-id-010-协程内持有全局锁跨-co_await-阻塞-io-线程并发请求死锁)
    - [2.3.3 [ID-011] uv_scheduler 并发设计陷阱（timer double-close / cancel 挂起 / WorkState 双重管理）](#233-id-011-uv_scheduler-并发设计陷阱timer-double-close--cancel-挂起--workstate-双重管理)

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
