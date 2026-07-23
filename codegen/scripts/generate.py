"""Generate C++ wire dispatch + Dart API from IR (sync/async/normal)."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

_SCRIPTS = Path(__file__).resolve().parent
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

from config_util import resolve_config  # noqa: E402
from parse_api import parse_project  # noqa: E402


def _dart_type(t: dict[str, Any]) -> str:
    k = t.get("kind")
    if k == "enum":
        return t["name"]
    if k == "optional":
        return f"{_dart_type(t['inner'])}?"
    return {
        "i32": "int",
        "u32": "int",
        "i64": "int",
        "bool": "bool",
        "string": "String",
        "void": "void",
    }.get(k, "dynamic")


def _cpp_call_expr(fn: dict[str, Any]) -> str:
    args = ", ".join(a["name"] for a in fn["args"] if a["type"].get("kind") != "stream_sink")
    # Leading :: avoids collision when wire lives in namespace dcb::demo.
    q = fn["qualified"]
    if not q.startswith("::"):
        q = "::" + q
    return f"{q}({args})"


def _cpp_read_arg(a: dict[str, Any]) -> str:
    k = a["type"].get("kind")
    name = a["name"]
    if k == "i32":
        return f"const auto {name} = r.i32();"
    if k == "u32":
        return f"const auto {name} = r.u32();"
    if k == "i64":
        return f"const auto {name} = r.i64();"
    if k == "string":
        return f"auto {name} = r.str();"
    if k == "bool":
        return f"const auto {name} = static_cast<bool>(r.u8());"
    if k == "enum":
        q = a["type"]["qualified"]
        if not q.startswith("::"):
            q = "::" + q
        return f"const auto {name} = static_cast<{q}>(r.i32());"
    if k == "optional":
        inner = a["type"]["inner"]
        ik = inner.get("kind")
        if ik == "i32":
            return f"const auto {name} = r.opt<std::int32_t>([&]() {{ return r.i32(); }});"
        if ik == "u32":
            return f"const auto {name} = r.opt<std::uint32_t>([&]() {{ return r.u32(); }});"
        if ik == "i64":
            return f"const auto {name} = r.opt<std::int64_t>([&]() {{ return r.i64(); }});"
        if ik == "bool":
            return f"const auto {name} = r.opt<bool>([&]() {{ return r.u8() != 0; }});"
        if ik == "string":
            return f"const auto {name} = r.opt<std::string>([&]() {{ return r.str(); }});"
        if ik == "enum":
            q = inner["qualified"]
            if not q.startswith("::"):
                q = "::" + q
            return f"const auto {name} = r.opt<{q}>([&]() {{ return static_cast<{q}>(r.i32()); }});"
        raise ValueError(f"unsupported optional inner type: {inner}")
    raise ValueError(f"unsupported arg type for codegen: {a}")


def _cpp_write_ret(t: dict[str, Any], expr: str) -> str:
    k = t.get("kind")
    if k == "void":
        return ""
    if k == "i32":
        return f"w.i32({expr});"
    if k == "u32":
        return f"w.u32({expr});"
    if k == "i64":
        return f"w.i64({expr});"
    if k == "string":
        return f"w.str({expr});"
    if k == "bool":
        return f"w.u8({expr} ? 1 : 0);"
    if k == "enum":
        return f"w.i32(static_cast<std::int32_t>({expr}));"
    if k == "optional":
        inner = t["inner"]
        ik = inner.get("kind")
        if ik == "i32":
            return f"w.opt({expr}, [&](const auto& v) {{ w.i32(v); }});"
        if ik == "u32":
            return f"w.opt({expr}, [&](const auto& v) {{ w.u32(v); }});"
        if ik == "i64":
            return f"w.opt({expr}, [&](const auto& v) {{ w.i64(v); }});"
        if ik == "bool":
            return f"w.opt({expr}, [&](const auto& v) {{ w.u8(v ? 1 : 0); }});"
        if ik == "string":
            return f"w.opt({expr}, [&](const auto& v) {{ w.str(v); }});"
        if ik == "enum":
            return f"w.opt({expr}, [&](const auto& v) {{ w.i32(static_cast<std::int32_t>(v)); }});"
        raise ValueError(f"unsupported optional inner type: {inner}")
    raise ValueError(f"unsupported return type: {t}")


def _dart_read_ret(t: dict[str, Any], expr: str) -> str:
    k = t.get("kind")
    if k == "void":
        return expr
    if k == "i32" or k == "u32":
        return f"ByteReader({expr}).i32()"
    if k == "i64":
        return f"ByteReader({expr}).i64()"
    if k == "string":
        return f"ByteReader({expr}).str()"
    if k == "bool":
        return f"ByteReader({expr}).u8() != 0"
    if k == "enum":
        return f"{t['name']}.values[ByteReader({expr}).i32()]"
    if k == "optional":
        inner = t["inner"]
        ik = inner.get("kind")
        read_value: str
        if ik == "i32" or ik == "u32":
            read_value = "_r.i32()"
        elif ik == "i64":
            read_value = "_r.i64()"
        elif ik == "bool":
            read_value = "_r.u8() != 0"
        elif ik == "string":
            read_value = "_r.str()"
        elif ik == "enum":
            read_value = f"{inner['name']}.values[_r.i32()]"
        else:
            raise ValueError(f"unsupported optional inner type: {inner}")
        return f"(() {{ final _r = ByteReader({expr}); final _has = _r.u8() != 0; return _has ? {read_value} : null; }})()"
    raise ValueError(f"unsupported dart return: {t}")


def _dart_write_args(args: list[dict[str, Any]]) -> str:
    lines = ["final _payload = ByteWriter();"]
    usable = [a for a in args if a["type"].get("kind") != "stream_sink"]
    if not usable:
        return "final _payloadBytes = Uint8List(0);"
    for a in usable:
        k = a["type"].get("kind")
        n = a["name"]
        if k == "i32" or k == "u32":
            lines.append(f"_payload.i32({n});")
        elif k == "i64":
            lines.append(f"_payload.i64({n});")
        elif k == "string":
            lines.append(f"_payload.str({n});")
        elif k == "bool":
            lines.append(f"_payload.u8({n} ? 1 : 0);")
        elif k == "enum":
            lines.append(f"_payload.i32({n}.index);")
        elif k == "optional":
            inner = a["type"]["inner"]
            ik = inner.get("kind")
            lines.append(f"if ({n} == null) {{ _payload.u8(0); }} else {{ _payload.u8(1);")
            if ik == "i32" or ik == "u32":
                lines.append(f"  _payload.i32({n});")
            elif ik == "i64":
                lines.append(f"  _payload.i64({n});")
            elif ik == "string":
                lines.append(f"  _payload.str({n});")
            elif ik == "bool":
                lines.append(f"  _payload.u8({n} ? 1 : 0);")
            elif ik == "enum":
                lines.append(f"  _payload.i32({n}.index);")
            else:
                raise ValueError(f"unsupported optional inner type: {inner}")
            lines.append("}")
        else:
            raise ValueError(f"unsupported dart arg: {a}")
    lines.append("final _payloadBytes = _payload.takeBytes();")
    return "\n    ".join(lines)


def generate_cpp(ir: dict[str, Any], api_includes: list[str]) -> tuple[str, str]:
    """Returns (hpp, cpp)."""
    fns = ir["functions"]
    hpp = """#pragma once
// GENERATED by dart_cpp_bridge codegen — do not edit.

#include "dart_cpp_bridge/session.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace dcb {
namespace demo {

void dispatch_request(std::shared_ptr<Session> session, std::uint64_t session_id,
                      const std::uint8_t* data, std::size_t len);
std::vector<std::uint8_t> dispatch_sync(const std::uint8_t* data, std::size_t len);

}  // namespace demo
}  // namespace dcb
"""
    inc_lines = "\n".join(f'#include "{p}"' for p in api_includes)
    cases = []
    sync_cases = []

    for fn in fns:
        mid = fn["method_id"]
        kind = fn["kind"]
        reads = "\n        ".join(_cpp_read_arg(a) for a in fn["args"] if a["type"].get("kind") != "stream_sink")
        call = _cpp_call_expr(fn)
        write = _cpp_write_ret(fn["return"], "out")

        if kind == "sync":
            body = f"""
      case {mid}: {{
        ByteReader r(frame.payload.data(), frame.payload.size());
        {reads}
        ByteWriter w;
        {{
          auto out = {call};
          {write}
        }}
        post_ok(session, gen, req, method, w.raw());
        break;
      }}"""
            cases.append(body)
            sync_body = f"""
  if (frame.method_id == {mid}u) {{
    ByteReader r(frame.payload.data(), frame.payload.size());
    {reads}
    ByteWriter w;
    {{
      auto out = {call};
      {write}
    }}
    return make_frame(MsgType::kResponseOk, frame.request_id, frame.method_id, w.raw());
  }}"""
            sync_cases.append(sync_body)

        elif kind == "async":
            capture_args = ", ".join(
                a["name"] for a in fn["args"] if a["type"].get("kind") != "stream_sink"
            )
            move_caps = ", ".join(
                f"{a['name']} = std::move({a['name']})"
                if a["type"].get("kind") == "string"
                else a["name"]
                for a in fn["args"]
                if a["type"].get("kind") != "stream_sink"
            )
            if move_caps:
                move_caps = ", " + move_caps
            body = f"""
      case {mid}: {{
        ByteReader r(frame.payload.data(), frame.payload.size());
        {reads}
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method{move_caps}]() -> async_simple::coro::Lazy<> {{
              try {{
                auto out = co_await {call};
                ByteWriter w;
                {write}
                post_ok(session, gen, req, method, w.raw());
              }} catch (const std::exception& e) {{
                post_err(session, gen, req, method, e.what());
              }} catch (...) {{
                post_err(session, gen, req, method, "unknown");
              }}
              co_return;
            }});
        break;
      }}"""
            cases.append(body)

        elif kind == "normal":
            move_caps = ", ".join(
                f"{a['name']} = std::move({a['name']})"
                if a["type"].get("kind") == "string"
                else a["name"]
                for a in fn["args"]
                if a["type"].get("kind") != "stream_sink"
            )
            if move_caps:
                lambda_extra = ", " + move_caps
            else:
                lambda_extra = ""
            body = f"""
      case {mid}: {{
        ByteReader r(frame.payload.data(), frame.payload.size());
        {reads}
        auto* io = &Runtime::instance().io();
        asio::post(Runtime::instance().pool(), [session, gen, req, method, io{lambda_extra}]() {{
          try {{
            auto out = {call};
            asio::post(*io, [session, gen, req, method, out = std::move(out)]() {{
              ByteWriter w;
              {write}
              post_ok(session, gen, req, method, w.raw());
            }});
          }} catch (const std::exception& e) {{
            asio::post(*io, [session, gen, req, method, msg = std::string(e.what())]() {{
              post_err(session, gen, req, method, msg);
            }});
          }} catch (...) {{
            asio::post(*io, [session, gen, req, method]() {{
              post_err(session, gen, req, method, "unknown");
            }});
          }}
        }});
        break;
      }}"""
            cases.append(body)
        else:
            raise ValueError(f"kind not supported yet: {kind}")

    cases_s = "\n".join(cases) if cases else ""
    sync_s = "\n".join(sync_cases) if sync_cases else ""

    cpp = f"""// GENERATED by dart_cpp_bridge codegen — do not edit.
#include "wire_dispatch.hpp"

#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"

{inc_lines}

#include <async_simple/coro/Lazy.h>

#include <asio/post.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dcb {{
namespace demo {{

namespace {{

void post_ok(const std::shared_ptr<Session>& s, std::uint64_t gen, std::uint64_t req,
             std::uint32_t method, const std::vector<std::uint8_t>& payload) {{
  s->try_post(gen, make_frame(MsgType::kResponseOk, req, method, payload));
}}

void post_err(const std::shared_ptr<Session>& s, std::uint64_t gen, std::uint64_t req,
              std::uint32_t method, const std::string& msg) {{
  ByteWriter w;
  w.i32(1);
  w.str(msg);
  s->try_post(gen, make_frame(MsgType::kResponseErr, req, method, w.raw()));
}}

}}  // namespace

void dispatch_request(std::shared_ptr<Session> session, std::uint64_t session_id,
                      const std::uint8_t* data, std::size_t len) {{
  const auto gen = session->generation();
  FrameHeader frame;
  try {{
    frame = parse_frame(data, len);
  }} catch (const std::exception& e) {{
    post_err(session, gen, 0, 0, std::string("bad frame: ") + e.what());
    return;
  }} catch (...) {{
    post_err(session, gen, 0, 0, "bad frame");
    return;
  }}

  const auto req = frame.request_id;
  const auto method = frame.method_id;

  try {{
    switch (method) {{
{cases_s}
      default:
        post_err(session, gen, req, method, "unknown method");
        break;
    }}
  }} catch (const std::exception& e) {{
    post_err(session, gen, req, method, e.what());
  }} catch (...) {{
    post_err(session, gen, req, method, "unknown");
  }}
}}

std::vector<std::uint8_t> dispatch_sync(const std::uint8_t* data, std::size_t len) {{
  auto frame = parse_frame(data, len);
{sync_s}
  throw std::runtime_error("sync: method not sync-capable");
}}

}}  // namespace demo
}}  // namespace dcb
"""
    return hpp, cpp


def _dart_fn_name(cpp_name: str) -> str:
    if "_" not in cpp_name:
        return cpp_name
    parts = cpp_name.split("_")
    return parts[0] + "".join(p.title() for p in parts[1:])


def _iter_dart_methods(ir: dict[str, Any]):
    for fn in ir["functions"]:
        dart_name = _dart_fn_name(fn["name"])
        params = []
        call_args = []
        for a in fn["args"]:
            if a["type"].get("kind") == "stream_sink":
                continue
            params.append(f"{_dart_type(a['type'])} {a['name']}")
            call_args.append(a["name"])
        ret_t = _dart_type(fn["return"])
        is_async = fn["kind"] != "sync"
        if is_async:
            sig_ret = "Future<void>" if ret_t == "void" else f"Future<{ret_t}>"
        else:
            sig_ret = ret_t
        yield {
            "fn": fn,
            "dart_name": dart_name,
            "param_s": ", ".join(params),
            "call_args": ", ".join(call_args),
            "ret_t": ret_t,
            "sig_ret": sig_ret,
            "is_async": is_async,
            "write_args": _dart_write_args(fn["args"]),
            "read_ret": _dart_read_ret(fn["return"], "_bytes"),
        }


def generate_dart_impl(ir: dict[str, Any], impl_class: str = "BridgeApiImpl") -> str:
    """Low-level generated impl (method ids + codec) — like FRB frb_generated."""
    methods = []
    id_consts = []
    for m in _iter_dart_methods(ir):
        dart_name = m["dart_name"]
        mid = m["fn"]["method_id"]
        id_consts.append(f"  static const int {dart_name}Id = {mid};")
        write_args = m["write_args"]
        param_s = m["param_s"]
        ret_t = m["ret_t"]
        read_ret = m["read_ret"]

        if not m["is_async"]:
            if ret_t == "void":
                body = f"""  {m['sig_ret']} {dart_name}({param_s}) {{
    {write_args}
    bridge.invokeSyncMethod({dart_name}Id, _payloadBytes);
  }}"""
            else:
                body = f"""  {m['sig_ret']} {dart_name}({param_s}) {{
    {write_args}
    final _bytes = bridge.invokeSyncMethod({dart_name}Id, _payloadBytes);
    return {read_ret};
  }}"""
        else:
            if ret_t == "void":
                body = f"""  {m['sig_ret']} {dart_name}({param_s}) async {{
    {write_args}
    await bridge.invokeAsyncMethod({dart_name}Id, _payloadBytes);
  }}"""
            else:
                body = f"""  {m['sig_ret']} {dart_name}({param_s}) async {{
    {write_args}
    final _bytes = await bridge.invokeAsyncMethod({dart_name}Id, _payloadBytes);
    return {read_ret};
  }}"""
        methods.append(body)

    methods_s = "\n\n".join(methods)
    ids_s = "\n".join(id_consts)

    enum_defs = []
    for e in sorted(ir.get("enums", []), key=lambda x: x["qualified"]):
        name = e["name"]
        values = ",\n  ".join(v["dart_name"] for v in e["values"])
        enum_defs.append(f"""/// Generated enum for `{e['qualified']}`.
enum {name} {{
  {values},
}}
""")
    enums_s = "\n".join(enum_defs)

    return f"""// GENERATED by dart_cpp_bridge codegen — do not edit.
// Low-level impl (method ids + wire). Prefer importing api.dart singleton.

import 'dart:typed_data';

import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';

{enums_s}
/// Wire-level API. Prefer [BridgeApi.instance] from `api.dart`.
final class {impl_class} {{
  {impl_class}(this.bridge);

  final DartCppBridge bridge;

{ids_s}

{methods_s}
}}
"""


def generate_dart_facade(
    ir: dict[str, Any],
    *,
    facade_class: str = "BridgeApi",
    impl_class: str = "BridgeApiImpl",
    impl_import: str = "api.g.dart",
) -> str:
    """Public singleton facade — thin forwarders to the impl (FRB-style)."""
    forwards = []
    for m in _iter_dart_methods(ir):
        dart_name = m["dart_name"]
        param_s = m["param_s"]
        call = m["call_args"]
        body = f"""  {m['sig_ret']} {dart_name}({param_s}) => _impl.{dart_name}({call});"""
        forwards.append(body)

    forwards_s = "\n".join(forwards)
    return f"""// GENERATED by dart_cpp_bridge codegen — do not edit.
// Public singleton facade (also used by top-level api_fn.dart).

import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';

export '{impl_import}';
import '{impl_import}';

/// App-facing API singleton.
///
/// Prefer top-level helpers in `api_fn.dart` for call sites:
/// `await initBridge(...); final v = bridgeVersion();`
final class {facade_class} {{
  {facade_class}._();

  static final {facade_class} instance = {facade_class}._();

  {impl_class}? _implOrNull;
  DartCppBridge? _bridgeOrNull;

  {impl_class} get _impl {{
    final impl = _implOrNull;
    if (impl == null) {{
      throw StateError('{facade_class}.init() must be called first');
    }}
    return impl;
  }}

  /// Underlying session bridge (after [init]).
  DartCppBridge get bridge {{
    final b = _bridgeOrNull;
    if (b == null) {{
      throw StateError('{facade_class}.init() must be called first');
    }}
    return b;
  }}

  bool get isInitialized => _implOrNull != null;

  /// Open the per-isolate session and bind generated impl.
  Future<void> init({{String? libraryPath}}) async {{
    if (_implOrNull != null) {{
      return;
    }}
    final b = await DartCppBridge.init(libraryPath: libraryPath);
    _bridgeOrNull = b;
    _implOrNull = {impl_class}(b);
  }}

  /// Prompt dispose of this isolate session (optional; finalizer also closes).
  void dispose() {{
    _bridgeOrNull?.dispose();
    _bridgeOrNull = null;
    _implOrNull = null;
  }}

  /// Process-wide runtime shutdown (main isolate / app exit only).
  void shutdown() {{
    _bridgeOrNull?.shutdown();
    _bridgeOrNull = null;
    _implOrNull = null;
  }}

{forwards_s}
}}
"""


def generate_dart_toplevel(
    ir: dict[str, Any],
    *,
    facade_class: str = "BridgeApi",
    facade_import: str = "api.dart",
) -> str:
    """Top-level functions that forward to the singleton (direct call style)."""
    lines = [
        f"""// GENERATED by dart_cpp_bridge codegen — do not edit.
// Top-level API — call like plain functions after initBridge().

import '{facade_import}';

/// Initialize the bridge (per isolate). Required before other top-level APIs.
Future<void> initBridge({{String? libraryPath}}) =>
    {facade_class}.instance.init(libraryPath: libraryPath);

/// Optional prompt session dispose.
void disposeBridge() => {facade_class}.instance.dispose();

/// Process-wide runtime shutdown (main isolate / app exit only).
void shutdownBridge() => {facade_class}.instance.shutdown();
"""
    ]
    for m in _iter_dart_methods(ir):
        dart_name = m["dart_name"]
        param_s = m["param_s"]
        call = m["call_args"]
        lines.append(
            f"""
{m['sig_ret']} {dart_name}({param_s}) =>
    {facade_class}.instance.{dart_name}({call});
"""
        )
    return "".join(lines)


def run_generate(config_path: Path) -> dict[str, Any]:
    result = parse_project(config_path)
    cfg = result["cfg"]
    ir = result["ir"]
    raw = cfg.get("raw") or {}

    impl_class = str(raw.get("dart_impl_class", "BridgeApiImpl"))
    facade_class = str(raw.get("dart_api_class", "BridgeApi"))
    impl_file = str(raw.get("dart_impl_file", "api.g.dart"))
    facade_file = str(raw.get("dart_api_file", "api.dart"))
    toplevel_file = str(raw.get("dart_fn_file", "api_fn.dart"))

    # API headers to include in wire: relative paths from cpp_root if possible
    api_includes: list[str] = []
    for h in ir["headers"]:
        hp = Path(h)
        try:
            rel = hp.relative_to(cfg["cpp_root"])
            api_includes.append(rel.as_posix())
        except ValueError:
            api_includes.append(hp.name)

    hpp, cpp = generate_cpp(ir, api_includes)
    dart_impl = generate_dart_impl(ir, impl_class=impl_class)
    dart_facade = generate_dart_facade(
        ir,
        facade_class=facade_class,
        impl_class=impl_class,
        impl_import=impl_file,
    )
    dart_fn = generate_dart_toplevel(
        ir,
        facade_class=facade_class,
        facade_import=facade_file,
    )

    cpp_out: Path = cfg["cpp_wire_output"]
    dart_out: Path = cfg["dart_output"]
    cpp_out.mkdir(parents=True, exist_ok=True)
    dart_out.mkdir(parents=True, exist_ok=True)

    (cpp_out / "wire_dispatch.hpp").write_text(hpp, encoding="utf-8")
    (cpp_out / "wire_dispatch.cpp").write_text(cpp, encoding="utf-8")
    (cpp_out / "ir.json").write_text(json.dumps(ir, indent=2) + "\n", encoding="utf-8")
    (dart_out / impl_file).write_text(dart_impl, encoding="utf-8")
    (dart_out / facade_file).write_text(dart_facade, encoding="utf-8")
    (dart_out / toplevel_file).write_text(dart_fn, encoding="utf-8")

    return {
        "functions": len(ir["functions"]),
        "cpp_out": str(cpp_out),
        "dart_out": str(dart_out),
        "dart_impl": impl_file,
        "dart_facade": facade_file,
        "dart_fn": toplevel_file,
        "ir": ir,
    }


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv:
        print("usage: generate.py <dart_cpp_bridge.yaml>", file=sys.stderr)
        return 2
    info = run_generate(Path(argv[0]))
    print(f"generated {info['functions']} functions")
    print(f"  C++         -> {info['cpp_out']}")
    print(f"  Dart impl   -> {info['dart_out']}/{info['dart_impl']}")
    print(f"  Dart facade -> {info['dart_out']}/{info['dart_facade']}")
    print(f"  Dart fn     -> {info['dart_out']}/{info['dart_fn']}")
    for fn in info["ir"]["functions"]:
        print(f"  - {fn['kind']:6} id={fn['method_id']} {fn['qualified']}")
    return 0 if info["functions"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
