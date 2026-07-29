// foreign_api.cpp — 业务实现：libuv worker 通过 ForeignExecutor + channel 与 bridge 通信。

#include "foreign_api.h"

#include "../uv_worker.hpp"

#include "dart_cpp_bridge/cbridge.h"
#include "dart_cpp_bridge/cbridge_wait.hpp"
#include "dart_cpp_bridge/channel.hpp"
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/dcb_codec.h"
#include "dart_cpp_bridge/foreign_executor.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace demo::api {

namespace {

std::mutex g_mu;
std::unique_ptr<UvWorker> g_uv_worker;

}  // namespace

async_simple::coro::Lazy<std::string> start_uv_worker() {
  std::lock_guard lock(g_mu);
  if (!g_uv_worker) {
    g_uv_worker = std::make_unique<UvWorker>("libuv-worker");
    g_uv_worker->start();
  }
  co_return std::string("uv worker started");
}

async_simple::coro::Lazy<std::string> stop_uv_worker() {
  std::lock_guard lock(g_mu);
  if (g_uv_worker) {
    g_uv_worker->stop();
    g_uv_worker.reset();
  }
  co_return std::string("uv worker stopped");
}

async_simple::coro::Lazy<std::string> ask_uv(std::string message) {
  auto [tx, rx] = co::oneshot::channel<std::string>();

  {
    std::lock_guard lock(g_mu);
    if (!g_uv_worker || !g_uv_worker->running()) {
      throw std::runtime_error("uv worker not running");
    }

    // 通过 ForeignExecutor::schedule 投递到 libuv loop 线程。
    // trampoline 在 uv loop 线程上执行 lambda → send 回 bridge。
    // 注意：std::function 要求可拷贝，所以用 shared_ptr 包装 move-only 的 Sender。
    auto* ex = g_uv_worker->executor();
    auto tx_ptr = std::make_shared<co::oneshot::Sender<std::string>>(std::move(tx));
    ex->schedule([tx_ptr, msg = std::move(message)]() {
      // 此代码在 libuv loop 线程上执行
      std::string result = "[uv:" + msg + "]";
      tx_ptr->send(std::move(result));  // 非阻塞 send，唤醒 bridge 侧协程
    });
  }

  // 在 bridge 主运行时上等待回复（挂起协程，不阻塞 io 线程）
  auto reply = co_await rx.recv();
  if (!reply) {
    throw std::runtime_error("uv worker dropped");
  }
  co_return *reply;
}

async_simple::coro::Lazy<std::int32_t> uv_compute(std::int32_t n) {
  auto [tx, rx] = co::oneshot::channel<std::int32_t>();

  {
    std::lock_guard lock(g_mu);
    if (!g_uv_worker || !g_uv_worker->running()) {
      throw std::runtime_error("uv worker not running");
    }

    auto* ex = g_uv_worker->executor();
    auto tx_ptr = std::make_shared<co::oneshot::Sender<std::int32_t>>(std::move(tx));
    ex->schedule([tx_ptr, n]() {
      // 在 libuv loop 线程上执行计算
      std::int32_t sum = 0;
      for (std::int32_t i = 1; i <= n; ++i) {
        sum += i;
      }
      tx_ptr->send(sum);
    });
  }

  auto reply = co_await rx.recv();
  if (!reply) {
    throw std::runtime_error("uv worker dropped");
  }
  co_return *reply;
}

void uv_stream(dcb::StreamSink<std::string> sink, std::int32_t count,
               std::int32_t interval_ms) {
  auto [tx, rx] = co::mpsc::unbounded<std::string>();

  {
    std::lock_guard lock(g_mu);
    if (!g_uv_worker || !g_uv_worker->running()) {
      sink.error("uv worker not running");
      return;
    }

    // 在 libuv loop 线程上启动发送任务
    // 注意：std::function 要求可拷贝，所以用 shared_ptr 包装 move-only 的 Sender。
    auto* ex = g_uv_worker->executor();
    auto tx_ptr = std::make_shared<co::mpsc::Sender<std::string>>(std::move(tx));
    ex->schedule([tx_ptr, count, interval_ms]() {
      // 在 uv loop 线程上执行（注意：sleep 会阻塞 uv loop，仅用于演示）
      for (std::int32_t i = 0; i < count; ++i) {
        if (interval_ms > 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
        if (!tx_ptr->send("uv_item_" + std::to_string(i))) {
          break;
        }
      }
      // tx_ptr 析构 → channel 关闭 → recv 返回 nullopt
    });
  }

  // 在 bridge 主运行时上消费 mpsc 并转发到 StreamSink
  dcb::Runtime::instance().spawn_on_asio(
      [sink = std::move(sink), rx = std::move(rx)]() mutable
      -> async_simple::coro::Lazy<> {
        while (true) {
          auto item = co_await rx.recv();
          if (!item) break;
          sink.add(*item);
        }
        sink.end();
        co_return;
      });
}

// MSVC 19.51 协程 lambda 捕获 bug 的 workaround：
// 使用独立的 static 协程函数，通过参数传递所有变量。
static async_simple::coro::Lazy<> uv_dart_fn_coro(
    std::shared_ptr<co::oneshot::Sender<std::string>> tx_ptr,
    dcb::DartFn<std::string(std::string)> cb,
    std::string input) {
  try {
    auto result = co_await cb(input);
    tx_ptr->send(std::move(result));
  } catch (const std::exception& e) {
    tx_ptr->send(std::string("ERROR: ") + e.what());
  }
  co_return;
}

async_simple::coro::Lazy<std::string> call_dart_from_uv(
    dcb::DartFn<std::string(std::string)> callback, std::string input) {
  auto [tx, rx] = co::oneshot::channel<std::string>();

  {
    std::lock_guard lock(g_mu);
    if (!g_uv_worker || !g_uv_worker->running()) {
      throw std::runtime_error("uv worker not running");
    }

    auto* ex = g_uv_worker->executor();
    auto tx_ptr = std::make_shared<co::oneshot::Sender<std::string>>(std::move(tx));

    // 在 uv loop 线程上启动协程，非阻塞地 co_await DartFn。
    // 使用 static 协程函数而非协程 lambda（MSVC 19.51 bug workaround）。
    ex->schedule([tx_ptr, cb = std::move(callback), input = std::move(input), ex]() mutable {
      uv_dart_fn_coro(std::move(tx_ptr), std::move(cb), std::move(input))
          .via(ex)
          .start([](auto&&) {});
    });
  }

  // 在 bridge 主运行时上等待结果
  auto reply = co_await rx.recv();
  if (!reply) {
    throw std::runtime_error("uv worker dropped");
  }
  co_return *reply;
}

// ─── cbridge 纯 C API 测试 ────────────────────────────────────────────────

async_simple::coro::Lazy<std::string> test_cbridge_async() {
  // 创建异步操作
  uint64_t op = dcb_async_create();

  // 启动一个线程，50ms 后从外部完成该操作（模拟外部 C 库回调）
  std::thread completer([op] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const char* msg = "cbridge_ok";
    dcb_async_complete(op, reinterpret_cast<const uint8_t*>(msg), 10);
  });
  completer.detach();

  // 协程非阻塞等待（挂起，不占线程）
  auto data = co_await dcb::async_wait(op);
  co_return std::string(data.begin(), data.end());
}

async_simple::coro::Lazy<std::string> test_cbridge_async_fail() {
  uint64_t op = dcb_async_create();

  std::thread failer([op] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    dcb_async_fail(op, "intentional_error");
  });
  failer.detach();

  try {
    co_await dcb::async_wait(op);
    co_return std::string("UNEXPECTED_SUCCESS");
  } catch (const std::exception& e) {
    co_return std::string("CAUGHT:") + e.what();
  }
}

async_simple::coro::Lazy<std::string> test_cbridge_async_cancel() {
  uint64_t op = dcb_async_create();

  std::thread canceller([op] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    dcb_async_cancel(op);
  });
  canceller.detach();

  try {
    co_await dcb::async_wait(op);
    co_return std::string("UNEXPECTED_SUCCESS");
  } catch (const std::exception& e) {
    co_return std::string("CAUGHT:") + e.what();
  }
}

async_simple::coro::Lazy<std::string> test_cbridge_invoke(
    dcb::DartFn<std::string(std::string)> callback, std::string input) {
  // 从 DartFn 提取 session_id 和 fn_id
  auto session = callback.session();
  if (!session) {
    throw std::runtime_error("test_cbridge_invoke: empty callback");
  }
  uint64_t session_id = dcb::SessionRegistry::instance().find_id(session);
  uint64_t fn_id = callback.fn_id();

  // 编码参数（使用纯 C codec API）
  dcb_writer cw;
  dcb_writer_init(&cw);
  dcb_write_str(&cw, input.c_str());

  // 在独立线程上调用纯 C API 并等待回调。
  // 不能在 io 线程上阻塞（回调在 io 线程触发，会死锁）。
  auto [promise, future] = []{
    std::promise<std::string> p;
    auto f = p.get_future();
    return std::make_pair(std::move(p), std::move(f));
  }();
  auto promise_ptr = std::make_shared<std::promise<std::string>>(std::move(promise));

  // 将 C writer 数据拷贝到 vector（writer 生命周期不跟线程）
  std::vector<uint8_t> args(cw.data, cw.data + cw.len);
  dcb_writer_free(&cw);

  std::thread worker([session_id, fn_id, args = std::move(args), promise_ptr] {
    struct Ctx {
      std::shared_ptr<std::promise<std::string>> p;
    };
    auto* ctx = new Ctx{promise_ptr};

    int rc = dcb_invoke_dart_fn(
        session_id, fn_id,
        args.data(), static_cast<uint32_t>(args.size()),
        [](void* ud, int ok, const uint8_t* data, uint32_t data_len, const char* error) {
          auto* c = static_cast<Ctx*>(ud);
          if (ok) {
            // 解码返回值（使用纯 C codec API）
            dcb_reader cr;
            dcb_reader_init(&cr, data, data_len);
            uint32_t slen = 0;
            const char* s = dcb_read_str(&cr, &slen);
            c->p->set_value(s ? std::string(s, slen) : std::string());
          } else {
            c->p->set_value(std::string("ERROR:") + (error ? error : "unknown"));
          }
          delete c;
        },
        ctx);

    if (rc != 0) {
      promise_ptr->set_value("ERROR:invoke_failed");
      delete ctx;
    }
  });

  // 在 io 线程上非阻塞等待结果（通过 spawn_blocking）
  auto result = co_await dcb::spawn_blocking([&future] {
    return future.get();
  });
  worker.join();
  co_return result;
}

// ─── 纯 C 路径的 dcb_invoke_dart_fn 测试 ───────────────────────────────
// 完全按照 cbridge.md 第三部分的模式：
//   C++ 协程提取 ID → dcb_async_create → 纯 C 函数调 dcb_invoke_dart_fn
//   → 纯 C 回调解码/编码/complete → 协程恢复

// 纯 C 上下文结构体（只用 malloc/free）
struct cbridge_pure_c_ctx {
  uint64_t op_id;
};

// 纯 C 回调：Dart 执行完成后由 bridge io 线程触发。
// 内部不使用任何 C++ 类型，仅用 dcb_codec + dcb_async_complete/fail。
static void on_dart_reply_pure_c(void* userdata, int ok, const uint8_t* data,
                                 uint32_t data_len, const char* error) {
  struct cbridge_pure_c_ctx* ctx = (struct cbridge_pure_c_ctx*)userdata;

  if (ok) {
    // 解码 Dart 返回的字符串（纯 C codec API）
    dcb_reader r;
    dcb_reader_init(&r, data, data_len);
    uint32_t slen = 0;
    const char* s = dcb_read_str(&r, &slen);

    // C 层处理：拼接 "C:<dart结果>"
    char result[512];
    int n = snprintf(result, sizeof(result), "C:%.*s", (int)slen, s ? s : "");
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(result)) n = (int)sizeof(result) - 1;

    // 编码结果，唤醒 C++ 协程
    dcb_writer w;
    dcb_writer_init(&w);
    dcb_write_str(&w, result);
    dcb_async_complete(ctx->op_id, w.data, w.len);
    dcb_writer_free(&w);
  } else {
    // Dart 抛了异常，转发给 C++ 协程
    dcb_async_fail(ctx->op_id, error ? error : "unknown dart error");
  }

  free(ctx);
}

// 纯 C 函数：编码参数并发起 dcb_invoke_dart_fn 调用。
// 可从任意线程调用（这里直接在 io 线程调用，因为 dcb_invoke_dart_fn 本身非阻塞）。
static void c_invoke_dart(uint64_t session_id, uint64_t fn_id,
                          uint64_t op_id, const char* input) {
  // 编码要传给 Dart 回调的参数
  dcb_writer w;
  dcb_writer_init(&w);
  dcb_write_str(&w, input);

  // 保存上下文（回调时需要 op_id）
  struct cbridge_pure_c_ctx* ctx =
      (struct cbridge_pure_c_ctx*)malloc(sizeof(struct cbridge_pure_c_ctx));
  ctx->op_id = op_id;

  // 发起调用（非阻塞，立即返回）
  int rc = dcb_invoke_dart_fn(session_id, fn_id,
                              w.data, w.len,
                              on_dart_reply_pure_c, ctx);
  dcb_writer_free(&w);

  if (rc != 0) {
    dcb_async_fail(op_id, "invoke failed: invalid session");
    free(ctx);
  }
}

async_simple::coro::Lazy<std::string> test_cbridge_invoke_pure_c(
    dcb::DartFn<std::string(std::string)> callback, std::string input) {
  // 1. 提取纯 C API 需要的 ID
  auto session = callback.session();
  if (!session) {
    throw std::runtime_error("test_cbridge_invoke_pure_c: empty callback");
  }
  uint64_t session_id = dcb::SessionRegistry::instance().find_id(session);
  uint64_t fn_id = callback.fn_id();

  // 2. 创建异步操作（C 层完成后用来唤醒本协程）
  uint64_t op_id = dcb_async_create();

  // 3. 调用纯 C 函数，C 层在回调中完成解码/编码/唤醒
  c_invoke_dart(session_id, fn_id, op_id, input.c_str());

  // 4. 挂起，等 C 层完成（不占 io 线程）
  auto payload = co_await dcb::async_wait(op_id);

  // 5. 解码 C 层返回的最终结果
  dcb::ByteReader r(payload.data(), payload.size());
  co_return r.str();
}

// ─── channel 服务模式测试 ───────────────────────────────────────────────

// 请求类型：数据 + 一次性回复通道
struct ServiceRequest {
  std::string payload;
  co::oneshot::Sender<std::string> reply_tx;
};

// 服务循环：在 uv worker 的 ForeignExecutor 上长期运行
static async_simple::coro::Lazy<> service_loop(co::mpsc::Receiver<ServiceRequest> rx) {
  while (auto req = co_await rx.recv()) {
    // 处理任务：加上前缀
    std::string result = "[svc:" + req->payload + "]";
    req->reply_tx.send(std::move(result));
  }
  co_return;  // channel 关闭，服务结束
}

async_simple::coro::Lazy<std::string> test_channel_service() {
  std::lock_guard lock(g_mu);
  if (!g_uv_worker || !g_uv_worker->running()) {
    throw std::runtime_error("uv worker not running");
  }

  // 创建 mpsc channel（bridge 侧发送，uv worker 侧接收）
  auto [tx, rx] = co::mpsc::unbounded<ServiceRequest>();

  // 在 uv worker 的 ForeignExecutor 上启动服务循环
  auto* ex = g_uv_worker->executor();
  service_loop(std::move(rx)).via(ex).start([](auto&&) {});

  // 从 bridge 侧发送 3 个请求，每个带独立的回复通道
  std::string results;
  for (int i = 0; i < 3; ++i) {
    auto [reply_tx, reply_rx] = co::oneshot::channel<std::string>();
    tx.send(ServiceRequest{"msg" + std::to_string(i), std::move(reply_tx)});

    // 非阻塞等待回复（挂起当前协程，不占 io 线程）
    auto reply = co_await reply_rx.recv();
    if (!reply) throw std::runtime_error("service dropped");
    if (!results.empty()) results += ",";
    results += *reply;
  }

  // 关闭 sender → 服务循环退出
  tx.close();
  co_return results;  // "[svc:msg0],[svc:msg1],[svc:msg2]"
}

// 并发版本：一次性发送所有请求，然后收集所有回复。
// 测试 mpsc 排队 + 服务循环逐个处理的能力。
async_simple::coro::Lazy<std::string> test_channel_service_concurrent() {
  std::lock_guard lock(g_mu);
  if (!g_uv_worker || !g_uv_worker->running()) {
    throw std::runtime_error("uv worker not running");
  }

  auto [tx, rx] = co::mpsc::unbounded<ServiceRequest>();
  auto* ex = g_uv_worker->executor();
  service_loop(std::move(rx)).via(ex).start([](auto&&) {});

  // 先一次性发送 5 个请求（不等待），测试 mpsc 排队
  std::vector<co::oneshot::Receiver<std::string>> receivers;
  for (int i = 0; i < 5; ++i) {
    auto [reply_tx, reply_rx] = co::oneshot::channel<std::string>();
    tx.send(ServiceRequest{"c" + std::to_string(i), std::move(reply_tx)});
    receivers.push_back(std::move(reply_rx));
  }

  // 然后收集所有回复（服务循环在 uv loop 线程上逐个处理）
  std::string results;
  for (auto& reply_rx : receivers) {
    auto reply = co_await reply_rx.recv();
    if (!reply) throw std::runtime_error("service dropped");
    if (!results.empty()) results += ",";
    results += *reply;
  }

  tx.close();
  co_return results;  // "[svc:c0],[svc:c1],[svc:c2],[svc:c3],[svc:c4]"
}

}  // namespace demo::api
