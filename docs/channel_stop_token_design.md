# Channel 取消语义与 stop token 改造设计

> 状态：已实施（2026-08-04，阶段 0–4 全部落地并通过验证；token 注入形态见 §4.5 末尾，exec::task 行为结论已回填 §4.6）
> 目标：把 `co::mpsc` 的 park 路径从「oneshot 信号 + 值托管给通道」改造为「tokio 式值所有权 + stdexec stop token 协作取消」，使撤回语义成立，且协作取消路径不再有 use-after-destroy 窗口。
> 范围：`dart/native/include/dart_cpp_bridge/channel.hpp`（`co::mpsc`）。`co::oneshot` 本体不动；C ABI / wire 协议 / Dart 公开 API 不变。

## 1. 背景与动机

channel 是本项目的核心基建：无论是把回调简单包装为异步操作，还是 actor 模型的消息驱动，都建立在 channel 的正确性与取消语义之上。

当前 stdexec 版 `co::oneshot` / `co::mpsc` 已经过三轮修复（见 `docs/known_issues.md` 的 ID-002 settle/start 发布顺序竞态、ID-003 取消丢值与 detached、ID-004 单消费者运行期强制），语义测试 19 例、`dcb_repro` 300 轮、smoke 全绿。但仍有三个结构性不足：

1. **发送侧取消不撤回值**（ID-003 修复时定为 accepted semantics）：park 那一刻值就 `move` 进了 `State::waiting_sends`，取消只断开唤醒信号，值随后仍被投递。发送方认为没发成，值却到了。
2. **取消只能靠「销毁 opstate」**：这不是 P2300 的协作式取消；且 `settle` 在锁外 `deliver` 与 opstate 并发析构之间存在纳秒级 UAF 窗口（ID-003 遗留）。
3. **没有 stop token 对接**：AGENTS.md 迁移规则第 5 条要求取消从 `Signal`/`Slot` 迁到 stop token（`inplace_stop_source`/`stop_token`），channel 是最大的一块缺失。

对标 tokio：`mpsc::Sender::send()` 的 future **持有值直到拿到 permit**，取消（drop future）即撤回，是 cancel-safe 的。本设计把同样的所有权模型落到 stdexec stop token 上：值在交付（claimed）之前始终属于发送方 opstate。

## 2. 目标与非目标

目标：

- **G1** `send()` / `recv()` 的 park 路径支持 stop token：stop 请求 → `set_stopped()` 完成，等待被撤回。
- **G2** tokio 式所有权：值在交付前属于发送方 opstate；取消 ⟹ 值不落通道（撤回语义）。
- **G3** 协作取消路径无 UAF 窗口：opstate 活到完成信号交付之后（P2300 契约之内）。
- **G4** 既有语义不变：FIFO、close/drain、单消费者运行期强制、`send` 的 bool 结果语义（`true` = 已接受，`false` = 通道关闭、值丢弃）。
- **G5** 既有测试基线不退化（撤回语义用例按新语义反转断言）。

非目标：

- Dart 侧取消 API（`task_id → stop_source` 映射、`cancelTask` 式接口）——属业务层决定，本设计只提供 C++ 地基。
- 多消费者（MPMC）支持——channel 维持单消费者契约。
- `co::oneshot` 的 stop token 化——DartFn 回执、cbridge pending ops 另行评估；oneshot 本体（含 `detached` 机制）保持不动。
- `exec::task` / scope 的 stop 传播改造——只验证 token 能传到通道，不改 task 本身。

## 3. 现状架构盘点

以 2026-08-04 工作区代码为准（`channel.hpp`）：

- `co::oneshot`：`State{mu, status, value, error, waiter, deliver, detached, settled}`；`Sender::settle` 锁内写值、最后发布 `settled`、取 waiter，锁外 `deliver`；`Receiver::opstate` 在 `start()` 注册 waiter / 走 settled 快路径，dtor 注销并置 `detached`。
- `co::mpsc::State`：`bq`（有界环形缓冲，rigtorp MPMCQueue，全部访问在 `mu` 下）、`uq`（无界 deque）、`pending_tx`（单个 parked 接收者的 `oneshot::Sender<T>`）、`waiting_sends`（`std::deque<WaitingSend{T value, oneshot::Sender<bool> signal}>`）。
- 两个关键结构事实：
  - **park 时值即移交通道**（`WaitingSend` 持有值）——撤回困难的根源；
  - **每次 park 构造一个 oneshot 通道做信号**——每次 park 一次 `make_shared<State>` 分配。
- 取消现状：销毁 opstate → oneshot dtor 注销 + `detached` → send 路径跳过 stale（ID-003）；撤回不发生。

## 4. 目标设计

### 4.1 核心结构变化

- `waiting_sends` 由 `std::deque<WaitingSend>` 改为**侵入式双向链表**：节点内嵌在 send opstate 内（驻留在调用方栈/协程帧上，零额外分配）。可参考 `third_party/stdexec/include/stdexec/__detail/__intrusive_queue.hpp`，但本场景全部操作在 `mu` 下，手写约 40 行双向链更直白。
- `pending_tx` 由 `std::optional<oneshot::Sender<T>>` 改为**单个 recv 节点指针**（单消费者：同时至多一个 parked recv，违规检测沿用 ID-004 的方式）。
- 节点为类型擦除基类（opstate 模板随 `Rcvr` 而变，队列不感知）：

```cpp
// mpsc::State 内的等待节点基类：内嵌于各 opstate，队列只持有指针。
struct wait_node {
  wait_node* prev = nullptr;
  wait_node* next = nullptr;
  // mu 下：从节点 move 出值（send 节点）/ 写入值（recv 节点），并标记已认领。
  // mu 外：完成回调（set_value / set_stopped）。
};
```

- 值在 park 期间留在 opstate 内（不进入 `State`）；认领时才 `move` 进 `bq` / 直投。

### 4.2 send opstate 状态机

所有状态翻转在 `State::mu` 下裁决；完成回调一律在 `mu` 外触发：

```text
kInit（start 入口，mu 下一次性裁决）
  ├─ 通道已关闭            → 立即 set_value(false)，值丢弃
  ├─ 有空位 / 有等待接收者 → 立即 set_value(true)（值当即入 bq / 直投）
  └─ 满                    → kQueued（节点挂入链尾）
kQueued
  ├─ recv/try_recv 认领    → kClaimed：mu 下把值 move 进 bq / 直投，mu 外 set_value(true)
  ├─ stop 请求             → kStopped：mu 下摘链，mu 外 set_stopped()，值不落通道（撤回）
  ├─ close                 → kClosed：mu 下摘链，mu 外 set_value(false)，值丢弃
  └─ opstate 销毁（兜底）  → mu 下摘链即销毁，无完成（见 4.7）
```

### 4.3 recv opstate 状态机

对称设计：

```text
kInit
  ├─ 有值可取（bq/uq/waiting_sends 直投）→ 立即 set_value(optional<T>)
  │    └─ 顺带认领链首 send 节点（沿用现唤醒逻辑，改为完成节点）
  ├─ 空且已关闭            → 立即 set_value(nullopt)
  └─ 空且未关闭            → kQueued（占用 recv 单槽）
kQueued
  ├─ send 直投认领         → mu 下写入值，mu 外 set_value(optional<T>)
  ├─ stop 请求             → mu 下摘槽，mu 外 set_stopped()
  ├─ close                 → mu 下摘槽，mu 外 set_value(nullopt)
  └─ opstate 销毁（兜底）  → mu 下摘槽即销毁，无完成
```

单消费者强制：recv 单槽已被一个活着的 kQueued 节点占用时，新的 `recv()` 抛 `std::logic_error`（同 ID-004 现状）；兜底摘除后槽位自然释放，不再有「stale 槽」概念。

### 4.4 事件裁决矩阵

| 当前状态 \ 事件 | 对端认领 | stop 请求 | close | opstate 销毁 |
| --- | --- | --- | --- | --- |
| kQueued(send) | 值入通道，`set_value(true)` | 摘链，`set_stopped`，**值撤回** | 摘链，`set_value(false)`，值丢弃 | 摘链，静默销毁 |
| kQueued(recv) | 直投，`set_value(v)` | 摘槽，`set_stopped` | `set_value(nullopt)` | 摘槽，静默销毁 |
| kClaimed | — | stop 认输（投递赢，与 tokio 一致） | 无效 | **契约外 UB（debug 断言辅助发现）** |

原则：状态翻转在 `mu` 下一次完成，谁先拿到锁谁赢；完成回调永不持锁触发。

### 4.5 stop callback 与锁序（本设计最需要小心的 50 行）

每个 opstate 持有：

```cpp
// 编译期取型（成员声明）：
using token_t    = stdexec::stop_token_of_t<stdexec::env_of_t<Rcvr>>;
using callback_t = stdexec::stop_callback_for_t<token_t, stop_fn>;
std::optional<callback_t> stop_reg_;

// 运行期取值（start() 内，时机见规则 1）：
auto token = stdexec::get_stop_token(stdexec::get_env(rcvr_));
```

命名空间是 `stdexec` 而非 `std::execution`（标准库 P2300 尚未在各编译器落地，本项目使用 vendored stdexec）。

已在 vendored 源码核实的两个保证：

- **env 不提供 stop token 时 `get_stop_token` 默认返回 `never_stop_token`**（`stop_token.hpp:115`：`__query<get_stop_token_t, never_stop_token{}, ...>`）。现有不传 token 的调用方（`sync_wait`、裸 receiver）零改动零成本，取消支持纯 opt-in。sender 链 / 协程内取 token 则用无参形式 `stdexec::read_env(stdexec::get_stop_token)`（`stop_token.hpp:430`），通道 opstate 内部用不上。
- **注销阻塞语义**（`stop_token.hpp:381-422` `__remove_callback_`）：回调正在其他线程执行时，注销自旋等待其完成；在通知线程的回调内部自注销不会死锁（`__removed_during_callback_` 标记）。这是规则 3 的实现依据。

硬性规则（写错即死锁 / UAF）：

1. **注册先于任何可完成路径**。`start()` 先 `stop_reg_.emplace(...)`，再进 `mu` 做 kInit 裁决。节点入队之前不可能有完成在飞，因此 emplace 期间（stop 已被请求过时回调会内联触发）是安全的；回调体内只置 `stop_requested_` 标志或在节点已入队后摘链。
2. 回调体：取 `mu`；若 `kQueued` → 摘链、置 kStopped、出锁后 `set_stopped()`；若 kInit → 仅置 `stop_requested_`（kInit 裁决看到标志后直接走 stopped 完成，不入队）；若 kClaimed → 什么也不做（认输）。
3. **注销（`stop_reg_.reset()`）严禁在持 `mu` 时进行**：`inplace_stop_callback` 的注销在回调正在其他线程执行时会阻塞至其返回，而回调要拿 `mu`——持锁注销即自死锁。完成路径统一在出锁后注销再完成。
4. 回调调用 `set_stopped()` 之后不得再触碰 opstate（完成即所有权移交对端）。
5. opstate dtor 析构 `stop_reg_` 同样不在 `mu` 下（同规则 3 的阻塞语义）。
6. `never_stop_token` 时 `stop_callback_for_t` 为空类型，注册零成本。

参考范式：`third_party/stdexec/include/exec/any_sender_of.hpp` 的 `__forward_stop_request` 注册方式。

锁序：整条 mpsc 路径只剩 `State::mu` 一把锁（oneshot 的 `mu` 退出 mpsc 路径），stop 回调体取 `mu`，无嵌套、无环。

**调用方注入 token 的形态（实施时已逐一验证，测试见 `examples/base_demo/test_channel.cpp` 的 stop token 区）：**

1. 自定义 receiver：`get_env()` 返回 `stdexec::env{stdexec::prop{stdexec::get_stop_token, tok}}`；opstate 在 `start()` 内用 `stdexec::get_stop_token(stdexec::get_env(rcvr_))` 取出（本设计 opstate 的取法）。
2. sender 链：`stdexec::write_env(tx.send(v), stdexec::prop{stdexec::get_stop_token, src.get_token()})`——write_env 的内部 receiver 会把 prop 合入其 env，下游 opstate 因此看到 token；不接 token 的现有调用方（裸 `sync_wait` / `then`）零改动零成本。
3. 协程（`exec::task`）：外层 receiver env 里的 token 经 task 自有 stop source 转发到 `co_await` 的 channel opstate（透传已验证）。注意 `exec::task` 顶层 `connect` 要求 env 提供调度器，用 `starts_on(inline_scheduler{}, write_env(task, prop{...}))` 这类组合。

**source 生命周期**：`inplace_stop_source` 必须活到所有注册其上的回调注销之后——即 opstate 先析构、source 后析构（测试用小作用域保证声明顺序）。

### 4.6 完成签名与调用方影响

- `send(v)`：`set_value_t(bool) + set_stopped_t`。不接入 stop 的现有调用方（`then` / `sync_wait`）行为完全不变；接入后 `sync_wait` 在 stopped 时返回空 optional，`std::get<0>(*...)` 会抛 `std::bad_optional_access`——接入 stop 的调用点必须显式处理（实施时做调用点审计：test / repro / runtime / 生成代码模板）。
- `recv()`：`set_value_t(std::optional<T>) + set_stopped_t`。`nullopt`（关闭）与 stopped（取消）从此是两条可区分的完成通道。
- `exec::task` 消费者（2026-08-04 已验证，回填）：`co_await` 的 sender 以 `set_stopped` 完成时，协程**不再恢复**——`as_awaitable` 桥接触发 promise 的 `unhandled_stopped()`，对称转移到外层续体，task 自身以 `set_stopped` 完成；`co_await` 之后的语句不执行，悬置帧随 opstate 销毁而析构。需要区分「关闭」与「取消」的调用方用 `stopped_as_optional` 或 receiver 的 `set_stopped` 分支。验证用例：`exec::task forwards stop into a parked channel await`。结论已同步至 `docs/cpp26_executor_model_usage.md` §8.7。

### 4.7 生命周期规则

- **协作路径（stop token）**：opstate 活到完成信号交付之后——无 UAF 窗口（G3）。
- **兜底路径（销毁即取消）**：`exec::task` 被 drop、测试直接析构 opstate 等场景继续支持——dtor 在 `mu` 下把仍 `kQueued` 的节点摘链即可安全销毁；撤回语义对销毁取消同样成立。
- **已 kClaimed 后销毁 = P2300 契约外 UB**：与所有 stdexec 算法同一保证级别；debug 构建加断言辅助发现。ID-003 的 UAF 窗口由此从「实现缺陷」收窄为「契约违规」。
- 运行时 teardown（如 Dart stream 被 drop）的纪律化方向：先 `request_stop`、drain、再销毁（对齐 AGENTS.md 既有坑注「scopes must be drained before destruction」）。

### 4.8 close 语义

不变：`closed` 置位；`bq`/`uq` 已缓冲的值仍可被 `recv()` 排空（drain-then-closed）；parked send 全部摘链 `set_value(false)`（值丢弃，同现语义）；parked recv 完成 `nullopt`。批量摘链在 `mu` 下，逐个完成在 `mu` 外。

### 4.9 与 oneshot 的关系

- `co::oneshot` 本体不动：DartFn 回执、cbridge pending ops 继续使用；`detached` 机制保留。
- mpsc 不再复用 oneshot 做 park 信号：`pending_tx` / `waiting_sends` 里的 oneshot 全部退役；连带红利——每次 park 省一次 `make_shared<State>` 分配。
- `ready()` / `ready_bool()` / `ready_closed()` 辅助随自定义 opstate 的内联完成而退役。

### 4.10 性能影响

- 减少：每次 park 的 oneshot `State` 分配（shared_ptr + mutex 构造）。
- 新增：每次 `start()` 的 stop callback 注册（`never_stop_token` 时零成本；inplace token 时为常数级指针操作）。
- 节点内嵌于 opstate，无额外堆分配；净效果预计不差于现状。以 `dcb_bench` 与吞吐用例红线（4 生产者 > 50000 msg/s）做回归卡控。

## 5. 实施步骤

每阶段独立可验证：`dcb_tests` 全绿 + `dcb_repro` 300 轮 + `dcb_smoke` 通过。

- **阶段 0：基线冻结**。记录当前 19 个用例清单；标注将反转语义用例（`mpsc bounded send cancel keeps the value in the channel`）。
- **阶段 1：send 路径节点化**。侵入式链表基础设施 + send opstate（值上移、认领、摘链、dtor 兜底），暂不接 stop token。撤回语义自此生效，反转上述用例断言。
- **阶段 2：recv 路径节点化**。recv opstate + 单槽 + 单消费者检查迁移；退役 mpsc 对 oneshot 的依赖（`pending_tx`、`ready_*` 辅助）。
- **阶段 3：stop token 接入**。send/recv opstate 按 4.5 注册 stop_callback；新增 §6 的确定性取消测试；exec::task 行为验证并回填 4.6。
- **阶段 4：清理与文档**。退役代码清理；`channel.hpp` 顶部注释更新；AGENTS.md 相关段落同步；`docs/known_issues.md` ID-003 遗留状态追加；bench 红线回归。

阶段 1/2 顺序可互换，建议先 send（撤回语义是本次改造的主要动机）。

实施记录（2026-08-04）：阶段 0–4 全部完成。每阶段以 `dcb_tests` + `dcb_repro` 300 轮 + `dcb_smoke` 验证通过；最终全套件 27 例（新增 7 个 stop token 确定性用例，覆盖 §6.1–6.5 全部计划项），`dcb_bench` 4 生产者有界 ≈32 万 msg/s（红线 5 万）。§6.4/6.5 的验证结论见 §4.5 末尾与 §4.6。

## 6. 测试计划

新增确定性测试（`stdexec::inplace_stop_source`，不再依赖时序巧合）：

1. send 因满 park → `request_stop()` → 调用方收到 `set_stopped`；值不在通道；FIFO 中后续 send 不受影响。
2. send park 与 recv 认领 / stop 并发压测：值要么在通道、要么撤回，绝不重复或丢失（账目守恒：accepted == received + withdrawn + closed_dropped）。
3. recv park → `request_stop()` → `set_stopped`；通道中的值不受影响；同通道后续 recv 正常。
4. exec::task env 透传专项：外层 receiver 携带 stop token，`co_await send()/recv()` 的协程内验证通道能收到 stop（验证 `stop_token_of_t<env_of_t<...>>` 链路）。
5. exec::task 对 `set_stopped` 的行为验证（await 结果形态），结论回填 4.6 与使用指南。
6. close 对 parked send/recv 的完成语义回归（false / nullopt）。
7. 既有 19 用例保持（其中 1 例按撤回语义反转）+ `dcb_repro` 300 轮 + `dcb_smoke` + `dcb_bench` 红线。

## 7. 风险与缓解

| 风险 | 等级 | 缓解 |
| --- | --- | --- |
| stop callback 注册/注销竞态写错 → 死锁 / UAF | 高 | 严格遵循 4.5 的硬性规则；对照 `any_sender_of.hpp` 的 `__forward_stop_request` 范式；专项竞态压测；评审 checklist 逐项过 |
| `exec::task` 对 `set_stopped` 的行为不符合预期 | 中 | 阶段 3 先写验证测试（§6.4/6.5），结论回填 4.6 再铺开 |
| 侵入式节点类型擦除错误 | 中 | 节点基类极小；所有节点操作集中在 `State` 内，评审聚焦 |
| 语义变更遗漏调用方 | 低 | `sync_wait` / `then` 调用点审计；`set_stopped` 只在接入 stop 后才出现 |
| 性能回退 | 低 | bench 红线卡控 |

## 8. 兼容性

- C ABI（`ffi.h` / `cbridge.h` / `foreign_runtime.h`）、wire 协议、Dart 公开 API：不动。
- C++ 头文件 `channel.hpp`：`send()` / `recv()` 完成签名**新增** `set_stopped_t`——additive，且通道本身仍是 stdexec 迁移中的未发布代码，符合兼容性政策。
- 回滚：按阶段 git revert；测试基线保证语义可追溯。

## 9. 工作量估算

1.5 ~ 2.5 天（含测试与评审）：节点基础设施 + send/recv 改造约 60%，stop callback 竞态与验证约 30%，文档与回归约 10%。

## 10. 参考资料

- `AGENTS.md`「Current initiative: async-simple → stdexec migration」规则 5（stop token）。
- `docs/cpp26_executor_model_usage.md`：本仓库 stdexec 用法基线。
- `docs/known_issues.md`：ID-002（settle/start 竞态）、ID-003（取消丢值与 detached）、ID-004（单消费者强制）。
- `third_party/stdexec/include/stdexec/stop_token.hpp`：`inplace_stop_source` / `inplace_stop_callback`。
- `third_party/stdexec/include/exec/any_sender_of.hpp`：`__forward_stop_request` 的 stop_callback 注册范式。
- `third_party/stdexec/include/stdexec/__detail/__intrusive_queue.hpp`：可参考的侵入式队列。
- tokio `mpsc::Sender::send` 文档：cancel-safety 语义对标。
