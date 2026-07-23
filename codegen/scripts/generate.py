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


def _lower_first(s: str) -> str:
    if not s:
        return s
    return s[0].lower() + s[1:]


def _cap_first(s: str) -> str:
    if not s:
        return s
    return s[0].upper() + s[1:]


def _class_impl_method_name(cls: dict[str, Any], method: dict[str, Any]) -> str:
    """Generated public method name on BridgeApiImpl for a class method."""
    prefix = _lower_first(cls["name"])
    if method["kind"] == "constructor":
        if not method["args"]:
            return f"{prefix}New"
        first = _dart_param_name(method["args"][0]["name"])
        return f"{prefix}NewWith{_cap_first(first)}"
    return f"{prefix}{_cap_first(_dart_fn_name(method['name']))}"


def _class_method_id_const_name(cls: dict[str, Any], method: dict[str, Any]) -> str:
    return _class_impl_method_name(cls, method) + "Id"


def _dart_type(t: dict[str, Any]) -> str:
    k = t.get("kind")
    if k == "enum":
        return t["name"]
    if k == "optional":
        return f"{_dart_type(t['inner'])}?"
    if k == "vector" or k == "array":
        return f"List<{_dart_type(t['inner'])}>"
    if k == "set":
        return f"Set<{_dart_type(t['inner'])}>"
    if k == "map":
        return f"Map<{_dart_type(t['key'])}, {_dart_type(t['value'])}>"
    if k == "i128" or k == "u128":
        return "BigInt"
    if k == "dart_fn":
        args = t.get("args", [])
        ret = _dart_type(t["return"])
        arg_types = ", ".join(_dart_type(a) for a in args)
        return f"FutureOr<{ret}> Function({arg_types})"
    if k in ("pair", "tuple"):
        elems = ", ".join(_dart_type(e) for e in t["elements"])
        return f"({elems})"
    if k == "data_class":
        return t["name"]
    if k == "opaque_class":
        return "int"
    return {
        "i32": "int",
        "u32": "int",
        "i64": "int",
        "bool": "bool",
        "string": "String",
        "void": "void",
        "f32": "double",
        "f64": "double",
    }.get(k, "dynamic")


def _cpp_call_expr(fn: dict[str, Any]) -> str:
    args = ", ".join(a["name"] for a in fn["args"] if a["type"].get("kind") != "stream_sink")
    # Leading :: avoids collision when wire lives in namespace dcb::demo.
    q = fn["qualified"]
    if not q.startswith("::"):
        q = "::" + q
    return f"{q}({args})"


def _cpp_type(t: dict[str, Any]) -> str:
    k = t.get("kind")
    if k == "i32":
        return "std::int32_t"
    if k == "u32":
        return "std::uint32_t"
    if k == "i64":
        return "std::int64_t"
    if k == "bool":
        return "bool"
    if k == "string":
        return "std::string"
    if k == "enum":
        q = t["qualified"]
        if not q.startswith("::"):
            q = "::" + q
        return q
    if k == "optional":
        return f"std::optional<{_cpp_type(t['inner'])}>"
    if k == "vector":
        return f"std::vector<{_cpp_type(t['inner'])}>"
    if k == "array":
        return f"std::array<{_cpp_type(t['inner'])}, {t['size']}>"
    if k == "set":
        return f"std::unordered_set<{_cpp_type(t['inner'])}>"
    if k == "map":
        return f"std::unordered_map<{_cpp_type(t['key'])}, {_cpp_type(t['value'])}>"
    if k == "i128":
        return "dcb::Int128"
    if k == "u128":
        return "dcb::UInt128"
    if k == "pair":
        elems = ", ".join(_cpp_type(e) for e in t["elements"])
        return f"std::pair<{elems}>"
    if k == "tuple":
        elems = ", ".join(_cpp_type(e) for e in t["elements"])
        return f"std::tuple<{elems}>"
    if k == "f32":
        return "float"
    if k == "f64":
        return "double"
    if k == "data_class":
        q = t["qualified"]
        if not q.startswith("::"):
            q = "::" + q
        return q
    if k == "opaque_class":
        q = t["qualified"]
        if not q.startswith("::"):
            q = "::" + q
        return q
    raise ValueError(f"unsupported C++ type: {t}")


def _cpp_write_item(t: dict[str, Any], expr: str) -> str:
    """Return a C++ statement that writes `expr` of type `t` using ByteWriter `w`."""
    k = t.get("kind")
    if k == "i32":
        return f"w.i32({expr});"
    if k == "u32":
        return f"w.u32({expr});"
    if k == "i64":
        return f"w.i64({expr});"
    if k == "bool":
        return f"w.u8({expr} ? 1 : 0);"
    if k == "string":
        return f"w.str({expr});"
    if k == "f32":
        return f"w.f32({expr});"
    if k == "f64":
        return f"w.f64({expr});"
    if k == "enum":
        return f"w.i32(static_cast<std::int32_t>({expr}));"
    if k == "data_class":
        return f"encode_{t['name']}(w, {expr});"
    if k in ("pair", "tuple"):
        helper = "pair" if k == "pair" else "tuple"
        writes = ", ".join(
            f"[&](const auto& v) {{ {_cpp_write_item(e, 'v')} }}"
            for e in t["elements"]
        )
        return f"w.{helper}({expr}, {writes});"
    raise ValueError(f"unsupported C++ item type: {t}")


def _cpp_read_item(t: dict[str, Any], reader: str = "r") -> str:
    """Return a C++ expression that reads one value of type `t` from ByteReader `reader`."""
    k = t.get("kind")
    if k == "i32":
        return f"{reader}.i32()"
    if k == "u32":
        return f"{reader}.u32()"
    if k == "i64":
        return f"{reader}.i64()"
    if k == "bool":
        return f"static_cast<bool>({reader}.u8())"
    if k == "string":
        return f"{reader}.str()"
    if k == "f32":
        return f"{reader}.f32()"
    if k == "f64":
        return f"{reader}.f64()"
    if k == "enum":
        q = t["qualified"]
        if not q.startswith("::"):
            q = "::" + q
        return f"static_cast<{q}>({reader}.i32())"
    if k == "data_class":
        return f"decode_{t['name']}({reader})"
    if k in ("pair", "tuple"):
        helper = "pair" if k == "pair" else "tuple"
        elem_types = ", ".join(_cpp_type(e) for e in t["elements"])
        reads = ", ".join(
            f"[&]() {{ return {_cpp_read_item(e, reader)}; }}"
            for e in t["elements"]
        )
        return f"{reader}.{helper}<{elem_types}>({reads})"
    raise ValueError(f"unsupported C++ item type: {t}")


def _data_class_type_quals(t: dict[str, Any]) -> set[str]:
    """Return qualified names of all data_class types referenced inside `t`."""
    k = t.get("kind")
    if k == "data_class":
        return {t["qualified"]}
    if k in ("vector", "array", "set", "optional"):
        return _data_class_type_quals(t["inner"])
    if k == "map":
        return _data_class_type_quals(t["key"]) | _data_class_type_quals(t["value"])
    if k in ("pair", "tuple"):
        result: set[str] = set()
        for e in t.get("elements", []):
            result.update(_data_class_type_quals(e))
        return result
    return set()


def _order_data_classes(classes: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Topologically sort data classes so dependencies are defined first."""
    by_q = {c["qualified"]: c for c in classes}
    visited: set[str] = set()
    ordered: list[dict[str, Any]] = []

    def visit(q: str, stack: set[str]) -> None:
        if q in visited:
            return
        if q in stack:
            raise ValueError(f"data_class cycle detected: {q}")
        stack.add(q)
        cls = by_q[q]
        for dep in sorted(_data_class_dependencies(cls)):
            if dep in by_q:
                visit(dep, stack)
        stack.remove(q)
        visited.add(q)
        ordered.append(cls)

    def _data_class_dependencies(cls: dict[str, Any]) -> set[str]:
        deps: set[str] = set()
        for f in cls.get("fields", []):
            deps.update(_data_class_type_quals(f["type"]))
        return deps

    for q in sorted(by_q):
        visit(q, set())
    return ordered


def _cpp_data_class_helpers(classes: list[dict[str, Any]]) -> str:
    """Generate inline encode/decode helpers for every data_class."""
    lines: list[str] = []
    for cls in _order_data_classes(classes):
        name = cls["name"]
        cpp_t = _cpp_type({"kind": "data_class", "qualified": cls["qualified"]})
        lines.append(f"inline void encode_{name}(ByteWriter& w, const {cpp_t}& v) {{")
        for f in cls["fields"]:
            field_expr = f"v.{f['name']}"
            lines.append(f"  {_cpp_write_item(f['type'], field_expr)}")
        lines.append("}")
        lines.append("")
        lines.append(f"inline {cpp_t} decode_{name}(ByteReader& r) {{")
        lines.append(f"  {cpp_t} v;")
        for f in cls["fields"]:
            lines.append(f"  v.{f['name']} = {_cpp_read_item(f['type'])};")
        lines.append("  return v;")
        lines.append("}")
    return "\n".join(lines)


def _cpp_class_method_cases(
    classes: list[dict[str, Any]],
) -> tuple[list[str], list[str]]:
    """Generate C++ dispatch cases for opaque class methods (constructor,
    instance, static)."""
    cases: list[str] = []
    sync_cases: list[str] = []

    for cls in classes:
        if cls.get("kind") != "opaque_class":
            continue
        class_q = cls["qualified"]
        if not class_q.startswith("::"):
            class_q = "::" + class_q
        class_name = cls["name"]

        for m in cls.get("methods", []):
            mid = m["method_id"]
            kind = m["kind"]
            is_static = m.get("is_static", False)
            is_constructor = kind == "constructor"
            non_sink_args = [
                a for a in m["args"] if a["type"].get("kind") != "stream_sink"
            ]
            arg_names = [a["name"] for a in non_sink_args]
            ret = m["return"]
            arg_reads = "\n        ".join(_cpp_read_arg(a) for a in non_sink_args)

            if is_constructor:
                ctor_call = f"std::make_shared<{class_q}>({', '.join(arg_names)})"
                body = f"""
      case {mid}: {{
        ByteReader r(frame.payload.data(), frame.payload.size());
        {arg_reads}
        auto obj = {ctor_call};
        const auto handle = dcb::ObjectHandleRegistry::instance().insert(session_id, obj, [](std::shared_ptr<void>&) {{}});
        ByteWriter w;
        w.u64(handle);
        post_ok(session, gen, req, method, w.raw());
        break;
      }}"""
                sync_body = f"""
  if (frame.method_id == {mid}u) {{
    ByteReader r(frame.payload.data(), frame.payload.size());
    {arg_reads}
    auto obj = {ctor_call};
    const auto handle = dcb::ObjectHandleRegistry::instance().insert(session_id, obj, [](std::shared_ptr<void>&) {{}});
    ByteWriter w;
    w.u64(handle);
    return make_frame(MsgType::kResponseOk, frame.request_id, frame.method_id, w.raw());
  }}"""
                cases.append(body)
                sync_cases.append(sync_body)
                continue

            err_msg = f"{class_name} handle not found or already dropped"
            if is_static:
                handle_block = arg_reads
                sync_handle_block = arg_reads
                call = f"{class_q}::{m['name']}({', '.join(arg_names)})"
            else:
                handle_block = f"""const auto handle = r.u64();
        auto obj = dcb::ObjectHandleRegistry::instance().get(handle);
        if (!obj) {{
          post_err(session, gen, req, method, "{err_msg}");
          break;
        }}
        {arg_reads}"""
                sync_handle_block = f"""const auto handle = r.u64();
        auto obj = dcb::ObjectHandleRegistry::instance().get(handle);
        if (!obj) {{
          ByteWriter ew;
          ew.i32(1);
          ew.str("{err_msg}");
          return make_frame(MsgType::kResponseErr, frame.request_id, frame.method_id, ew.raw());
        }}
        {arg_reads}"""
                call = f"static_cast<{class_q}*>(obj.get())->{m['name']}({', '.join(arg_names)})"

            write = _cpp_write_ret(ret, "out") if kind != "stream" else ""

            if kind == "sync":
                body = f"""
      case {mid}: {{
        ByteReader r(frame.payload.data(), frame.payload.size());
        {handle_block}
        ByteWriter w;
        {{
          auto out = {call};
          {write}
        }}
        post_ok(session, gen, req, method, w.raw());
        break;
      }}"""
                sync_body = f"""
  if (frame.method_id == {mid}u) {{
    ByteReader r(frame.payload.data(), frame.payload.size());
    {sync_handle_block}
    ByteWriter w;
    {{
      auto out = {call};
      {write}
    }}
    return make_frame(MsgType::kResponseOk, frame.request_id, frame.method_id, w.raw());
  }}"""
                cases.append(body)
                sync_cases.append(sync_body)

            elif kind == "async":
                move_caps = ", ".join(
                    f"{a['name']} = std::move({a['name']})"
                    if a["type"].get("kind") == "string"
                    else a["name"]
                    for a in non_sink_args
                )
                handle_cap = "handle, obj, " if not is_static else ""
                if move_caps:
                    captures = handle_cap + move_caps
                else:
                    captures = handle_cap.rstrip(", ")
                call_stmt = f"co_await {call};" if ret.get("kind") == "void" else f"auto out = co_await {call};"
                body = f"""
      case {mid}: {{
        ByteReader r(frame.payload.data(), frame.payload.size());
        {handle_block}
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method, session_id, {captures}]() -> async_simple::coro::Lazy<> {{
              try {{
                {call_stmt}
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
                    for a in non_sink_args
                )
                handle_cap = "handle, obj, " if not is_static else ""
                if move_caps:
                    lambda_extra = ", " + handle_cap + move_caps
                else:
                    lambda_extra = ", " + handle_cap.rstrip(", ") if handle_cap else ""
                call_stmt = call + ";" if ret.get("kind") == "void" else f"auto out = {call};"
                body = f"""
      case {mid}: {{
        ByteReader r(frame.payload.data(), frame.payload.size());
        {handle_block}
        auto* io = &Runtime::instance().io();
        asio::post(Runtime::instance().pool(), [session, gen, req, method, io, session_id{lambda_extra}]() {{
          try {{
            {call_stmt}
            asio::post(*io, [session, gen, req, method, session_id, out = std::move(out)]() {{
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

            elif kind == "stream":
                sink_arg = next(
                    a for a in m["args"] if a["type"].get("kind") == "stream_sink"
                )
                sink_inner = sink_arg["type"]["inner"]
                sink_encode = _cpp_write_item(sink_inner, "v")
                call_arg_exprs = []
                for a in m["args"]:
                    if a["type"].get("kind") == "stream_sink":
                        call_arg_exprs.append("std::move(sink)")
                    else:
                        call_arg_exprs.append(a["name"])
                stream_call = (
                    f"static_cast<{class_q}*>(obj.get())->{m['name']}({', '.join(call_arg_exprs)})"
                    if not is_static
                    else f"{class_q}::{m['name']}({', '.join(call_arg_exprs)})"
                )
                body = f"""
      case {mid}: {{
        ByteReader r(frame.payload.data(), frame.payload.size());
        {handle_block}
        auto sink = dcb::StreamSink<{_cpp_type(sink_inner)}>(session.get(), req, gen, method, []({_cpp_type(sink_inner)} v) {{
          ByteWriter w;
          {sink_encode}
          return w.raw();
        }});
        {stream_call};
        break;
      }}"""
                cases.append(body)

            else:
                raise ValueError(f"kind not supported yet: {kind}")

    return cases, sync_cases


def _dart_data_class_defs(classes: list[dict[str, Any]]) -> str:
    """Generate immutable Dart data classes with const constructors."""
    defs: list[str] = []
    for cls in classes:
        name = cls["name"]
        fields = cls["fields"]
        ctor_lines = []
        field_lines = []
        equals_parts = []
        hash_fields = []
        for f in fields:
            dart_name = _dart_param_name(f["name"])
            ctor_lines.append(f"    required this.{dart_name},")
            field_lines.append(f"  final {_dart_type(f['type'])} {dart_name};")
            equals_parts.append(
                f"        {dart_name} == other.{dart_name}"
            )
            hash_fields.append(dart_name)
        ctor_s = "\n".join(ctor_lines)
        field_s = "\n".join(field_lines)
        equals_s = " &&\n".join(equals_parts) if equals_parts else "        true"
        hash_args = ", ".join(hash_fields) if hash_fields else "0"
        defs.append(
            f"""/// Generated data class for `{cls['qualified']}`.
final class {name} {{
  const {name}({{
{ctor_s}
  }});

{field_s}

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      (other is {name} &&
{equals_s});

  @override
  int get hashCode => Object.hash({hash_args});
}}"""
        )
    return "\n\n".join(defs)


def _dart_data_class_helpers(classes: list[dict[str, Any]]) -> str:
    """Generate Dart encode/decode helpers for every data_class."""
    lines: list[str] = []
    for cls in classes:
        name = cls["name"]
        fields = cls["fields"]
        lines.append(f"void _writeDataClass_{name}(ByteWriter w, {name} v) {{")
        for f in fields:
            dart_name = _dart_param_name(f["name"])
            lines.extend(
                _dart_write_item(f["type"], f"v.{dart_name}", indent="  ", writer="w")
            )
        lines.append("}")
        lines.append("")
        lines.append(f"{name} _readDataClass_{name}(ByteReader _r) {{")
        lines.append(f"  return {name}(")
        for f in fields:
            dart_name = _dart_param_name(f["name"])
            lines.append(
                f"    {dart_name}: {_dart_read_item(f['type'], '_r')},"
            )
        lines.append("  );")
        lines.append("}")
    return "\n".join(lines)


def _cpp_read_arg(a: dict[str, Any]) -> str:
    t = a["type"]
    k = t.get("kind")
    name = a["name"]
    if k in ("i32", "u32", "i64", "bool", "string", "enum", "f32", "f64", "data_class"):
        return f"const auto {name} = {_cpp_read_item(t)};"
    if k == "optional":
        inner = t["inner"]
        inner_t = _cpp_type(inner)
        return f"const auto {name} = r.opt<{inner_t}>([&]() {{ return {_cpp_read_item(inner)}; }});"
    if k == "vector":
        inner = t["inner"]
        inner_t = _cpp_type(inner)
        return f"const auto {name} = r.vec<{inner_t}>([&]() {{ return {_cpp_read_item(inner)}; }});"
    if k == "array":
        inner = t["inner"]
        size = t["size"]
        inner_t = _cpp_type(inner)
        return f"const auto {name} = r.arr<{inner_t}, {size}>([&]() {{ return {_cpp_read_item(inner)}; }});"
    if k == "set":
        inner = t["inner"]
        inner_t = _cpp_type(inner)
        return f"const auto {name} = r.set<{inner_t}>([&]() {{ return {_cpp_read_item(inner)}; }});"
    if k == "map":
        key_t = _cpp_type(t["key"])
        value_t = _cpp_type(t["value"])
        return (
            f"const auto {name} = r.map<{key_t}, {value_t}>("
            f"[&]() {{ return {_cpp_read_item(t['key'])}; }}, "
            f"[&]() {{ return {_cpp_read_item(t['value'])}; }});"
        )
    if k == "i128":
        return f"const auto {name} = r.i128();"
    if k == "u128":
        return f"const auto {name} = r.u128();"
    if k in ("pair", "tuple"):
        helper = "pair" if k == "pair" else "tuple"
        elem_types = ", ".join(_cpp_type(e) for e in t["elements"])
        reads = ", ".join(
            f"[&]() {{ return {_cpp_read_item(e)}; }}"
            for e in t["elements"]
        )
        return f"const auto {name} = r.{helper}<{elem_types}>({reads});"

    if k == "opaque_class":
        q = t["qualified"]
        if not q.startswith("::"):
            q = "::" + q
        return (
            f"const auto {name}Handle = r.u64();\n"
            f"        auto {name}Obj = dcb::ObjectHandleRegistry::instance().get({name}Handle);\n"
            f"        if (!{name}Obj) {{\n"
            f"          post_err(session, gen, req, method, \"{t['name']} handle not found or already dropped\");\n"
            f"          break;\n"
            f"        }}\n"
            f"        {q}& {name} = *static_cast<{q}*>({name}Obj.get());"
        )

    if k == "dart_fn":
        sig_ret = _cpp_type(t["return"])
        sig_args = [_cpp_type(a) for a in t.get("args", [])]
        signature = f"{sig_ret}({', '.join(sig_args)})"
        arg_names = [f"a{i}" for i in range(len(sig_args))]
        arg_decls = ", ".join(
            f"const {ty}& {an}" for ty, an in zip(sig_args, arg_names)
        )
        if arg_decls:
            arg_decls = ", " + arg_decls
        encode_body = "\n        ".join(
            _cpp_write_item(arg_ir, an) for arg_ir, an in zip(t.get("args", []), arg_names)
        )
        if encode_body:
            encode_body = "\n        " + encode_body
        ret_kind = t["return"].get("kind")
        if ret_kind == "void":
            decode_body = "(void)d; (void)n;"
        else:
            decode_body = f"ByteReader r(d, n);\n        return {_cpp_read_item(t['return'], 'r')};"
        return f"""const auto {name} = dcb::DartFn<{signature}>(session, gen, r.u64(),
    [](ByteWriter& w{arg_decls}) {{{encode_body}
    }},
    [](const std::uint8_t* d, std::size_t n) {{
      {decode_body}
    }});"""
    raise ValueError(f"unsupported arg type for codegen: {a}")


def _cpp_write_ret(t: dict[str, Any], expr: str) -> str:
    k = t.get("kind")
    if k == "void":
        return ""
    if k in ("i32", "u32", "i64", "bool", "string", "enum", "f32", "f64", "data_class"):
        return _cpp_write_item(t, expr)
    if k == "optional":
        inner = t["inner"]
        item = _cpp_write_item(inner, "v")
        return f"w.opt({expr}, [&](const auto& v) {{ {item} }});"
    if k == "vector":
        inner = t["inner"]
        item = _cpp_write_item(inner, "v")
        return f"w.vec({expr}, [&](const auto& v) {{ {item} }});"
    if k == "array":
        inner = t["inner"]
        item = _cpp_write_item(inner, "v")
        return f"w.arr({expr}, [&](const auto& v) {{ {item} }});"
    if k == "set":
        inner = t["inner"]
        item = _cpp_write_item(inner, "v")
        return f"w.set({expr}, [&](const auto& v) {{ {item} }});"
    if k == "map":
        key_item = _cpp_write_item(t["key"], "k")
        value_item = _cpp_write_item(t["value"], "v")
        return (
            f"w.map({expr}, "
            f"[&](const auto& k) {{ {key_item} }}, "
            f"[&](const auto& v) {{ {value_item} }});"
        )
    if k == "i128":
        return f"w.i128({expr});"
    if k == "u128":
        return f"w.u128({expr});"
    if k in ("pair", "tuple"):
        helper = "pair" if k == "pair" else "tuple"
        writes = ", ".join(
            f"[&](const auto& v) {{ {_cpp_write_item(e, 'v')} }}"
            for e in t["elements"]
        )
        return f"w.{helper}({expr}, {writes});"
    if k == "opaque_class":
        q = t["qualified"]
        if not q.startswith("::"):
            q = "::" + q
        return (
            f"{{ auto __obj = std::make_shared<{q}>(std::move({expr})); "
            f"const auto __handle = dcb::ObjectHandleRegistry::instance().insert("
            f"session_id, __obj, [](std::shared_ptr<void>&) {{}}); w.u64(__handle); }}"
        )
    raise ValueError(f"unsupported return type: {t}")


def _dart_write_item(
    t: dict[str, Any],
    expr: str,
    indent: str = "",
    writer: str = "_payload",
) -> list[str]:
    """Return Dart statement(s) that write `expr` of type `t` using ByteWriter `writer`."""
    k = t.get("kind")
    if k == "i32":
        return [f"{indent}{writer}.i32({expr});"]
    if k == "u32":
        return [f"{indent}{writer}.u32({expr});"]
    if k == "i64":
        return [f"{indent}{writer}.i64({expr});"]
    if k == "bool":
        return [f"{indent}{writer}.u8({expr} ? 1 : 0);"]
    if k == "string":
        return [f"{indent}{writer}.str({expr});"]
    if k == "f32":
        return [f"{indent}{writer}.f32({expr});"]
    if k == "f64":
        return [f"{indent}{writer}.f64({expr});"]
    if k == "enum":
        return [f"{indent}{writer}.i32({expr}.index);"]
    if k == "data_class":
        return [f"{indent}_writeDataClass_{t['name']}({writer}, {expr});"]
    if k == "opaque_class":
        return [f"{indent}{writer}.u64({expr});"]
    if k == "i128":
        return [f"{indent}{writer}.writeI128({expr});"]
    if k == "u128":
        return [f"{indent}{writer}.writeU128({expr});"]
    if k == "optional":
        inner = t["inner"]
        return [
            f"{indent}if ({expr} == null) {{ {writer}.u8(0); }} else {{ {writer}.u8(1);",
            *_dart_write_item(inner, expr, indent + "  ", writer),
            f"{indent}}}",
        ]
    if k == "vector":
        inner = t["inner"]
        return [
            f"{indent}{writer}.u32({expr}.length);",
            f"{indent}for (final _v in {expr}) {{",
            *_dart_write_item(inner, "_v", indent + "  ", writer),
            f"{indent}}}",
        ]
    if k == "array":
        inner = t["inner"]
        size = t["size"]
        return [
            f"{indent}if ({expr}.length != {size}) throw StateError('array length mismatch');",
            f"{indent}for (final _v in {expr}) {{",
            *_dart_write_item(inner, "_v", indent + "  ", writer),
            f"{indent}}}",
        ]
    if k == "set":
        inner = t["inner"]
        return [
            f"{indent}{writer}.u32({expr}.length);",
            f"{indent}for (final _v in {expr}) {{",
            *_dart_write_item(inner, "_v", indent + "  ", writer),
            f"{indent}}}",
        ]
    if k == "map":
        key_t = t["key"]
        value_t = t["value"]
        return [
            f"{indent}{writer}.u32({expr}.length);",
            f"{indent}{expr}.forEach((final _k, final _v) {{",
            *_dart_write_item(key_t, "_k", indent + "  ", writer),
            *_dart_write_item(value_t, "_v", indent + "  ", writer),
            f"{indent}}});",
        ]
    if k in ("pair", "tuple"):
        lines = []
        for i, e in enumerate(t["elements"], start=1):
            lines.extend(_dart_write_item(e, f"{expr}.${i}", indent, writer))
        return lines
    raise ValueError(f"unsupported Dart item type: {t}")


def _dart_read_item(t: dict[str, Any], reader: str = "_r") -> str:
    """Return a Dart expression that reads one value of type `t` from ByteReader `reader`."""
    k = t.get("kind")
    if k == "i32":
        return f"{reader}.i32()"
    if k == "u32":
        return f"{reader}.u32()"
    if k == "i64":
        return f"{reader}.i64()"
    if k == "bool":
        return f"{reader}.u8() != 0"
    if k == "string":
        return f"{reader}.str()"
    if k == "f32":
        return f"{reader}.f32()"
    if k == "f64":
        return f"{reader}.f64()"
    if k == "enum":
        return f"{t['name']}.values[{reader}.i32()]"
    if k == "data_class":
        return f"_readDataClass_{t['name']}({reader})"
    if k == "opaque_class":
        return f"{reader}.u64()"
    if k == "u128":
        return f"{reader}.readU128()"
    if k == "optional":
        inner = t["inner"]
        read_value = _dart_read_item(inner, reader)
        return f"(({reader}.u8() != 0) ? {read_value} : null)"
    if k == "vector":
        inner = t["inner"]
        item_type = _dart_type(inner)
        item_read = _dart_read_item(inner, reader)
        return f"(() {{ final _n = {reader}.u32(); final _result = <{item_type}>[]; for (var _i = 0; _i < _n; _i++) {{ _result.add({item_read}); }} return _result; }})()"
    if k == "array":
        inner = t["inner"]
        size = t["size"]
        item_type = _dart_type(inner)
        item_read = _dart_read_item(inner, reader)
        return f"(() {{ final _result = <{item_type}>[]; for (var _i = 0; _i < {size}; _i++) {{ _result.add({item_read}); }} return _result; }})()"
    if k == "set":
        inner = t["inner"]
        item_type = _dart_type(inner)
        item_read = _dart_read_item(inner, reader)
        return f"(() {{ final _n = {reader}.u32(); final _result = <{item_type}>{{}}; for (var _i = 0; _i < _n; _i++) {{ _result.add({item_read}); }} return _result; }})()"
    if k == "map":
        key_type = _dart_type(t["key"])
        value_type = _dart_type(t["value"])
        key_read = _dart_read_item(t["key"], reader)
        value_read = _dart_read_item(t["value"], reader)
        return f"(() {{ final _n = {reader}.u32(); final _result = <{key_type}, {value_type}>{{}}; for (var _i = 0; _i < _n; _i++) {{ _result[{key_read}] = {value_read}; }} return _result; }})()"
    if k in ("pair", "tuple"):
        n = len(t["elements"])
        elem_reads = ", ".join(_dart_read_item(e, "_r") for e in t["elements"])
        if n == 1:
            elem_reads += ","
        return f"(() {{ final _r = {reader}; return ({elem_reads}); }})()"
    raise ValueError(f"unsupported Dart item type: {t}")


def _dart_read_ret(t: dict[str, Any], expr: str) -> str:
    k = t.get("kind")
    if k == "void":
        return expr
    if k == "i32":
        return f"ByteReader({expr}).i32()"
    if k == "u32":
        return f"ByteReader({expr}).u32()"
    if k == "i64":
        return f"ByteReader({expr}).i64()"
    if k == "string":
        return f"ByteReader({expr}).str()"
    if k == "bool":
        return f"ByteReader({expr}).u8() != 0"
    if k == "f32":
        return f"ByteReader({expr}).f32()"
    if k == "f64":
        return f"ByteReader({expr}).f64()"
    if k == "enum":
        return f"{t['name']}.values[ByteReader({expr}).i32()]"
    if k == "data_class":
        return f"_readDataClass_{t['name']}(ByteReader({expr}))"
    if k == "opaque_class":
        return f"ByteReader({expr}).u64()"
    if k == "optional":
        inner = t["inner"]
        read_value = _dart_read_item(inner, "_r")
        return f"(() {{ final _r = ByteReader({expr}); final _has = _r.u8() != 0; return _has ? {read_value} : null; }})()"
    if k == "vector":
        inner = t["inner"]
        item_type = _dart_type(inner)
        item_read = _dart_read_item(inner, "_r")
        return f"(() {{ final _r = ByteReader({expr}); final _n = _r.u32(); final _result = <{item_type}>[]; for (var _i = 0; _i < _n; _i++) {{ _result.add({item_read}); }} return _result; }})()"
    if k == "array":
        inner = t["inner"]
        size = t["size"]
        item_type = _dart_type(inner)
        item_read = _dart_read_item(inner, "_r")
        return f"(() {{ final _r = ByteReader({expr}); final _result = <{item_type}>[]; for (var _i = 0; _i < {size}; _i++) {{ _result.add({item_read}); }} return _result; }})()"
    if k == "set":
        inner = t["inner"]
        item_type = _dart_type(inner)
        item_read = _dart_read_item(inner, "_r")
        return f"(() {{ final _r = ByteReader({expr}); final _n = _r.u32(); final _result = <{item_type}>{{}}; for (var _i = 0; _i < _n; _i++) {{ _result.add({item_read}); }} return _result; }})()"
    if k == "map":
        key_type = _dart_type(t["key"])
        value_type = _dart_type(t["value"])
        key_read = _dart_read_item(t["key"], "_r")
        value_read = _dart_read_item(t["value"], "_r")
        return f"(() {{ final _r = ByteReader({expr}); final _n = _r.u32(); final _result = <{key_type}, {value_type}>{{}}; for (var _i = 0; _i < _n; _i++) {{ _result[{key_read}] = {value_read}; }} return _result; }})()"
    if k == "i128":
        return f"ByteReader({expr}).readI128()"
    if k == "u128":
        return f"ByteReader({expr}).readU128()"
    if k in ("pair", "tuple"):
        n = len(t["elements"])
        elem_reads = ", ".join(_dart_read_item(e, "_r") for e in t["elements"])
        if n == 1:
            elem_reads += ","
        return f"(() {{ final _r = ByteReader({expr}); return ({elem_reads}); }})()"
    raise ValueError(f"unsupported dart return: {t}")


def _dart_payload_lines(args: list[dict[str, Any]]) -> list[str]:
    """Return Dart payload-construction lines (excluding `_payloadBytes` and
    stream_sink arguments, which are handled at call site).

    DartFn arguments are written as `_payload.u64(_<name>Id)` in their original
    parameter order; the caller must register the closure and assign the id
    variable before these lines run."""
    lines = ["final _payload = ByteWriter();"]
    for a in args:
        t = a["type"]
        k = t.get("kind")
        if k == "stream_sink":
            continue
        n = a["dart_name"]
        if k == "dart_fn":
            lines.append(f"_payload.u64(_{n}Id);")
        elif k == "opaque_class":
            lines.append(f"_payload.u64({n});")
        elif k in ("i32", "u32", "i64", "string", "bool", "enum", "f32", "f64", "data_class"):
            lines.extend(_dart_write_item(t, n))
        elif k == "optional":
            inner = t["inner"]
            lines.append(f"if ({n} == null) {{ _payload.u8(0); }} else {{ _payload.u8(1);")
            lines.extend(_dart_write_item(inner, n, "  "))
            lines.append("}")
        elif k == "vector":
            inner = t["inner"]
            lines.append(f"_payload.u32({n}.length);")
            lines.append(f"for (final _v in {n}) {{")
            lines.extend(_dart_write_item(inner, "_v", "  "))
            lines.append("}")
        elif k == "array":
            inner = t["inner"]
            size = t["size"]
            lines.append(f"if ({n}.length != {size}) throw StateError('array length mismatch');")
            lines.append(f"for (final _v in {n}) {{")
            lines.extend(_dart_write_item(inner, "_v", "  "))
            lines.append("}")
        elif k == "set":
            inner = t["inner"]
            lines.append(f"_payload.u32({n}.length);")
            lines.append(f"for (final _v in {n}) {{")
            lines.extend(_dart_write_item(inner, "_v", "  "))
            lines.append("}")
        elif k == "map":
            key_t = t["key"]
            value_t = t["value"]
            lines.append(f"_payload.u32({n}.length);")
            lines.append(f"{n}.forEach((final _k, final _v) {{")
            lines.extend(_dart_write_item(key_t, "_k", "  "))
            lines.extend(_dart_write_item(value_t, "_v", "  "))
            lines.append("});")
        elif k == "i128":
            lines.append(f"_payload.writeI128({n});")
        elif k == "u128":
            lines.append(f"_payload.writeU128({n});")
        elif k in ("pair", "tuple"):
            lines.extend(_dart_write_item(t, n))
        else:
            raise ValueError(f"unsupported dart arg: {a}")
    return lines


def _dart_opaque_class_wrappers(classes: list[dict[str, Any]]) -> str:
    """Generate user-facing Dart wrapper classes for each opaque C++ class."""

    def wrapper_type(t: dict[str, Any]) -> str:
        if t.get("kind") == "opaque_class":
            return t["name"]
        return _dart_type(t)

    def build_params(args: list[dict[str, Any]], named: bool) -> str:
        parts: list[str] = []
        for a in args:
            t = a["type"]
            if t.get("kind") == "stream_sink":
                continue
            dn = _dart_param_name(a["name"])
            if t.get("kind") == "dart_fn":
                arg_types = ", ".join(_dart_type(arg_t) for arg_t in t.get("args", []))
                ret_t = _dart_type(t["return"])
                p = f"FutureOr<{ret_t}> Function({arg_types}) {dn}"
            else:
                p = f"{wrapper_type(t)} {dn}"
            if a.get("default_value"):
                p = f"{p} = {a['default_value']}"
            parts.append(p)
        required = [p for p in parts if " = " not in p]
        optional = [p for p in parts if " = " in p]
        if named:
            named_required = [f"required {p}" for p in required]
            all_named = named_required + optional
            if all_named:
                return "{" + ", ".join(all_named) + "}"
            return ""
        # positional optional
        if required and optional:
            return ", ".join(required) + ", [" + ", ".join(optional) + "]"
        if optional:
            return "[" + ", ".join(optional) + "]"
        return ", ".join(required)

    wrappers: list[str] = []
    for cls in classes:
        if cls.get("kind") != "opaque_class":
            continue
        class_name = cls["name"]
        ctor_lines: list[str] = []
        method_lines: list[str] = []

        for method in cls.get("methods", []):
            if method["kind"] == "constructor":
                param_s = build_params(method["args"], named=True)
                impl_name = _class_impl_method_name(cls, method)
                call_args = []
                for a in method["args"]:
                    t = a["type"]
                    dn = _dart_param_name(a["name"])
                    if t.get("kind") == "opaque_class":
                        call_args.append(f"{dn}.handle")
                    else:
                        call_args.append(dn)
                call = f"BridgeApi.instance.{impl_name}({', '.join(call_args)})"
                if not method["args"]:
                    factory_name = class_name
                else:
                    first = _dart_param_name(method["args"][0]["name"])
                    factory_name = f"{class_name}.with{_cap_first(first)}"
                ctor_lines.append(
                    f"  factory {factory_name}({param_s}) => {class_name}._("
                    f"bridge: BridgeApi.instance.bridge, handle: {call});"
                )
                continue

            is_static = method.get("is_static", False)
            is_stream = method["kind"] == "stream"
            is_async = method["kind"] not in ("sync",)
            dart_name = _dart_fn_name(method["name"])
            ret = method["return"]

            if is_stream:
                sink_arg = next(
                    a for a in method["args"] if a["type"].get("kind") == "stream_sink"
                )
                item_t = sink_arg["type"]["inner"]
                ret_t = _dart_type(item_t)
                sig_ret = f"Stream<{ret_t}>"
            elif ret.get("kind") == "opaque_class":
                ret_t = ret["name"]
                sig_ret = f"Future<{ret_t}>" if is_async else ret_t
            else:
                ret_t = _dart_type(ret)
                if is_async:
                    sig_ret = "Future<void>" if ret_t == "void" else f"Future<{ret_t}>"
                else:
                    sig_ret = ret_t

            param_s = build_params(method["args"], named=False)

            call_args: list[str] = []
            if not is_static:
                call_args.append("handle")
            for a in method["args"]:
                t = a["type"]
                dn = _dart_param_name(a["name"])
                if t.get("kind") == "stream_sink":
                    continue
                if t.get("kind") == "opaque_class":
                    call_args.append(f"{dn}.handle")
                else:
                    call_args.append(dn)

            impl_name = _class_impl_method_name(cls, method)
            call = f"BridgeApi.instance.{impl_name}({', '.join(call_args)})"

            if is_static:
                if ret.get("kind") == "opaque_class":
                    body = (
                        f"final newHandle = {'await ' if is_async else ''}{call};\n"
                        f"    return {ret['name']}._(bridge: BridgeApi.instance.bridge, handle: newHandle);"
                    )
                else:
                    body = f"return {call};"
                method_lines.append(
                    f"  static {sig_ret} {dart_name}({param_s}) "
                    f"{'async ' if is_async and not is_stream else ''}{{\n    {body}\n  }}"
                )
            else:
                if is_stream:
                    body = f"return {call};"
                elif ret.get("kind") == "opaque_class":
                    body = (
                        f"final newHandle = await {call};\n"
                        f"    return {ret['name']}._(bridge: BridgeApi.instance.bridge, handle: newHandle);"
                    )
                elif is_async:
                    body = f"return await {call};" if ret_t != "void" else f"await {call};"
                else:
                    body = f"return {call};"
                method_lines.append(
                    f"  {sig_ret} {dart_name}({param_s}) "
                    f"{'async ' if is_async and not is_stream else ''}{{\n"
                    f"    ensureAlive();\n    {body}\n  }}"
                )

        ctor_s = "\n".join(ctor_lines)
        method_s = "\n".join(method_lines)
        wrappers.append(
            f"""final class {class_name} extends CppOpaqueInterface {{
  {class_name}._({{required super.bridge, required super.handle}});

{ctor_s}

{method_s}
}}"""
        )

    return "\n\n".join(wrappers)


def generate_cpp(ir: dict[str, Any], api_includes: list[str]) -> tuple[str, str]:
    """Returns (hpp, cpp)."""
    fns = ir["functions"]
    data_classes = [c for c in ir.get("classes", []) if c.get("kind") == "data_class"]
    cpp_helpers = _cpp_data_class_helpers(data_classes)
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
std::vector<std::uint8_t> dispatch_sync(std::uint64_t session_id, const std::uint8_t* data, std::size_t len);

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

        elif kind == "stream":
            non_sink_args = [a for a in fn["args"] if a["type"].get("kind") != "stream_sink"]
            reads = "\n        ".join(_cpp_read_arg(a) for a in non_sink_args)
            sink_arg = next(a for a in fn["args"] if a["type"].get("kind") == "stream_sink")
            sink_inner = sink_arg["type"]["inner"]
            sink_encode = _cpp_write_item(sink_inner, "v")
            call_arg_exprs = []
            for a in fn["args"]:
                if a["type"].get("kind") == "stream_sink":
                    call_arg_exprs.append("std::move(sink)")
                else:
                    call_arg_exprs.append(a["name"])
            q = fn["qualified"]
            if not q.startswith("::"):
                q = "::" + q
            call = f"{q}({', '.join(call_arg_exprs)})"
            body = f"""
      case {mid}: {{
        ByteReader r(frame.payload.data(), frame.payload.size());
        {reads}
        auto sink = dcb::StreamSink<{_cpp_type(sink_inner)}>(session.get(), req, gen, method, []({_cpp_type(sink_inner)} v) {{
          ByteWriter w;
          {sink_encode}
          return w.raw();
        }});
        {call};
        break;
      }}"""
            cases.append(body)

        else:
            raise ValueError(f"kind not supported yet: {kind}")

    class_cases, class_sync_cases = _cpp_class_method_cases(ir.get("classes", []))
    cases.extend(class_cases)
    sync_cases.extend(class_sync_cases)

    cases_s = "\n".join(cases) if cases else ""
    sync_s = "\n".join(sync_cases) if sync_cases else ""

    cpp = f"""// GENERATED by dart_cpp_bridge codegen — do not edit.
#include "wire_dispatch.hpp"

#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/object_handle.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

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

{cpp_helpers}

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

std::vector<std::uint8_t> dispatch_sync(std::uint64_t session_id, const std::uint8_t* data, std::size_t len) {{
  auto frame = parse_frame(data, len);
{sync_s}
  throw std::runtime_error("sync: method not sync-capable");
}}

}}  // namespace demo
}}  // namespace dcb
"""
    return hpp, cpp


def _dart_param_name(cpp_name: str) -> str:
    """Convert a C++ parameter/argument name to Dart camelCase."""
    if "_" not in cpp_name:
        return cpp_name
    parts = cpp_name.split("_")
    return parts[0] + "".join(p.title() for p in parts[1:])


def _dart_fn_name(cpp_name: str) -> str:
    if "_" not in cpp_name:
        return cpp_name
    parts = cpp_name.split("_")
    return parts[0] + "".join(p.title() for p in parts[1:])


def _iter_dart_methods(ir: dict[str, Any]):
    for fn in ir["functions"]:
        dart_name = _dart_fn_name(fn["name"])
        args = []
        for a in fn["args"]:
            args.append({**a, "dart_name": _dart_param_name(a["name"])})
        params = []
        call_args = []
        for a in args:
            if a["type"].get("kind") == "stream_sink":
                continue
            params.append(f"{_dart_type(a['type'])} {a['dart_name']}")
            call_args.append(a["dart_name"])
        ret_t = _dart_type(fn["return"])
        is_stream = fn["kind"] == "stream"
        is_async = not is_stream and fn["kind"] != "sync"
        stream_decode_expr = None
        if is_stream:
            sink_args = [a for a in args if a["type"].get("kind") == "stream_sink"]
            if not sink_args:
                raise ValueError(f"stream method without StreamSink arg: {fn['qualified']}")
            item_t = sink_args[0]["type"]["inner"]
            ret_t = _dart_type(item_t)
            sig_ret = f"Stream<{ret_t}>"
            stream_decode_expr = _dart_read_item(item_t, "_r")
        elif is_async:
            sig_ret = "Future<void>" if ret_t == "void" else f"Future<{ret_t}>"
        else:
            sig_ret = ret_t
        dart_fn_args = [a for a in args if a["type"].get("kind") == "dart_fn"]
        yield {
            "fn": fn,
            "dart_name": dart_name,
            "param_s": ", ".join(params),
            "call_args": ", ".join(call_args),
            "ret_t": ret_t,
            "sig_ret": sig_ret,
            "is_async": is_async,
            "is_stream": is_stream,
            "payload_lines": _dart_payload_lines(args),
            "dart_fn_args": dart_fn_args,
            "read_ret": _dart_read_ret(fn["return"], "_bytes"),
            "stream_decode_expr": stream_decode_expr,
        }


def _dart_fn_wrapper_lines(a: dict[str, Any]) -> list[str]:
    """Generate Dart code that wraps a typed user closure into a binary
    callback suitable for [DartCppBridge.registerDartFn]."""
    t = a["type"]
    name = a["dart_name"]
    wrapper_name = f"_{name}Wrapper"
    args = t.get("args", [])
    ret = t["return"]
    lines = [
        f"final {wrapper_name} = (Uint8List _argBytes) async {{",
        "  final _r = ByteReader(_argBytes);",
    ]
    arg_names = []
    for i, arg in enumerate(args):
        an = f"_a{i}"
        arg_names.append(an)
        lines.append(f"  final {an} = {_dart_read_item(arg, '_r')};")
    lines.append(f"  final _res = await {name}({', '.join(arg_names)});")
    if ret.get("kind") == "void":
        lines.add("  return Uint8List(0);")
    else:
        lines.append("  final _w = ByteWriter();")
        for stmt in _dart_write_item(ret, "_res", indent="  ", writer="_w"):
            lines.append(stmt)
        lines.append("  return _w.takeBytes();")
    lines.append("};")
    lines.append(f"final _{name}Id = bridge.registerDartFn({wrapper_name});")
    return lines


def generate_dart_impl(ir: dict[str, Any], impl_class: str = "BridgeApiImpl") -> str:
    """Low-level generated impl (method ids + codec) — like FRB frb_generated."""
    methods = []
    id_consts = []
    for m in _iter_dart_methods(ir):
        dart_name = m["dart_name"]
        mid = m["fn"]["method_id"]
        id_consts.append(f"  static const int {dart_name}Id = {mid};")
        param_s = m["param_s"]
        ret_t = m["ret_t"]
        read_ret = m["read_ret"]
        payload_lines = m["payload_lines"]
        dart_fn_args = m["dart_fn_args"]

        # Build payload body. DartFn callbacks must be registered before the
        # payload is sent and unregistered after the C++ call returns.
        body_lines: list[str] = []
        dart_fn_try = bool(dart_fn_args)
        if dart_fn_try:
            if m["is_stream"]:
                raise ValueError(
                    f"DartFn callbacks inside stream methods are not supported: {fn['qualified']}"
                )
            for a in dart_fn_args:
                body_lines.extend(_dart_fn_wrapper_lines(a))
            body_lines.append("try {")
            indent = "  "
        else:
            indent = ""

        if m["is_stream"]:
            for line in payload_lines:
                body_lines.append(f"{indent}{line}")
            body_lines.append(
                f"{indent}return bridge.openStream<{ret_t}>({dart_name}Id, _payload.takeBytes(), "
                f"(final _r) => {m['stream_decode_expr']});"
            )
        else:
            for line in payload_lines:
                body_lines.append(f"{indent}{line}")
            body_lines.append(f"{indent}final _payloadBytes = _payload.takeBytes();")
            if not m["is_async"]:
                if ret_t == "void":
                    body_lines.append(f"{indent}bridge.invokeSyncMethod({dart_name}Id, _payloadBytes);")
                else:
                    body_lines.append(f"{indent}final _bytes = bridge.invokeSyncMethod({dart_name}Id, _payloadBytes);")
                    body_lines.append(f"{indent}return {read_ret};")
            else:
                if ret_t == "void":
                    body_lines.append(f"{indent}await bridge.invokeAsyncMethod({dart_name}Id, _payloadBytes);")
                else:
                    body_lines.append(f"{indent}final _bytes = await bridge.invokeAsyncMethod({dart_name}Id, _payloadBytes);")
                    body_lines.append(f"{indent}return {read_ret};")

        if dart_fn_try:
            body_lines.append("} finally {")
            for a in dart_fn_args:
                body_lines.append(f"  bridge.unregisterDartFn(_{a['dart_name']}Id);")
            body_lines.append("}")

        body_inner = "\n    ".join(body_lines)
        is_async = m["is_async"]
        body = f"""  {m['sig_ret']} {dart_name}({param_s}) {'async ' if is_async else ''}{{
    {body_inner}
  }}"""
        methods.append(body)

    # Opaque class low-level methods
    opaque_classes = [c for c in ir.get("classes", []) if c.get("kind") == "opaque_class"]
    for cls in opaque_classes:
        for method in cls.get("methods", []):
            for a in method["args"]:
                a.setdefault("dart_name", _dart_param_name(a["name"]))

            impl_name = _class_impl_method_name(cls, method)
            mid = method["method_id"]
            id_consts.append(f"  static const int {_class_method_id_const_name(cls, method)} = {mid};")
            is_constructor = method["kind"] == "constructor"
            is_static = method.get("is_static", False)
            is_instance = not is_constructor and not is_static
            is_stream = method["kind"] == "stream"

            # Build params and call args for the impl method.
            params = []
            call_args = []
            if is_instance:
                params.append("int handle")
                call_args.append("handle")
            for a in method["args"]:
                t = a["type"]
                if t.get("kind") == "stream_sink":
                    continue
                dart_name = a["dart_name"]
                if t.get("kind") == "opaque_class":
                    params.append(f"int {dart_name}")
                elif t.get("kind") == "dart_fn":
                    arg_types = ", ".join(_dart_type(arg_t) for arg_t in t.get("args", []))
                    ret_t = _dart_type(t["return"])
                    params.append(f"FutureOr<{ret_t}> Function({arg_types}) {dart_name}")
                else:
                    params.append(f"{_dart_type(t)} {dart_name}")
                call_args.append(dart_name)
            param_s = ", ".join(params)

            # Payload construction (handle first for instance methods).
            payload_lines = ["final _payload = ByteWriter();"]
            if is_instance:
                payload_lines.append("_payload.u64(handle);")
            for a in method["args"]:
                t = a["type"]
                n = a["dart_name"]
                k = t.get("kind")
                if k == "stream_sink":
                    continue
                if k == "dart_fn":
                    payload_lines.append(f"_payload.u64(_{n}Id);")
                elif k == "opaque_class":
                    payload_lines.append(f"_payload.u64({n});")
                else:
                    payload_lines.extend(_dart_write_item(t, n))

            # Determine return type handling.
            ret_type = method["return"]
            if is_constructor or ret_type.get("kind") == "opaque_class":
                ret_t = "int"
                read_ret = "ByteReader(_bytes).u64()"
            else:
                ret_t = _dart_type(ret_type)
                read_ret = _dart_read_ret(ret_type, "_bytes")

            if is_stream:
                sink_args = [a for a in method["args"] if a["type"].get("kind") == "stream_sink"]
                if not sink_args:
                    raise ValueError(f"stream method without StreamSink: {cls['qualified']}::{method['name']}")
                item_t = sink_args[0]["type"]["inner"]
                ret_t = _dart_type(item_t)
                sig_ret = f"Stream<{ret_t}>"
                stream_decode_expr = _dart_read_item(item_t, "_r")
                is_async = False
            else:
                is_async = method["kind"] not in ("sync", "constructor")
                sig_ret = "Future<void>" if is_async and ret_t == "void" else (f"Future<{ret_t}>" if is_async else ret_t)

            dart_fn_args = [a for a in method["args"] if a["type"].get("kind") == "dart_fn"]

            body_lines: list[str] = []
            dart_fn_try = bool(dart_fn_args)
            if dart_fn_try:
                if is_stream:
                    raise ValueError(
                        f"DartFn callbacks inside stream methods are not supported: {cls['qualified']}::{method['name']}"
                    )
                for a in dart_fn_args:
                    body_lines.extend(_dart_fn_wrapper_lines(a))
                body_lines.append("try {")
                indent = "  "
            else:
                indent = ""

            if is_stream:
                for line in payload_lines:
                    body_lines.append(f"{indent}{line}")
                body_lines.append(
                    f"{indent}return bridge.openStream<{ret_t}>({impl_name}Id, _payload.takeBytes(), "
                    f"(final _r) => {stream_decode_expr});"
                )
            else:
                for line in payload_lines:
                    body_lines.append(f"{indent}{line}")
                body_lines.append(f"{indent}final _payloadBytes = _payload.takeBytes();")
                if not is_async:
                    if ret_t == "void":
                        body_lines.append(f"{indent}bridge.invokeSyncMethod({impl_name}Id, _payloadBytes);")
                    else:
                        body_lines.append(f"{indent}final _bytes = bridge.invokeSyncMethod({impl_name}Id, _payloadBytes);")
                        body_lines.append(f"{indent}return {read_ret};")
                else:
                    if ret_t == "void":
                        body_lines.append(f"{indent}await bridge.invokeAsyncMethod({impl_name}Id, _payloadBytes);")
                    else:
                        body_lines.append(f"{indent}final _bytes = await bridge.invokeAsyncMethod({impl_name}Id, _payloadBytes);")
                        body_lines.append(f"{indent}return {read_ret};")

            if dart_fn_try:
                body_lines.append("} finally {")
                for a in dart_fn_args:
                    body_lines.append(f"  bridge.unregisterDartFn(_{a['dart_name']}Id);")
                body_lines.append("}")

            body_inner = "\n    ".join(body_lines)
            async_keyword = "async " if is_async else ""
            body = f"""  {sig_ret} {impl_name}({param_s}) {async_keyword}{{
    {body_inner}
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

    data_classes = [
        c for c in ir.get("classes", []) if c.get("kind") == "data_class"
    ]
    data_class_defs = _dart_data_class_defs(data_classes)
    if data_class_defs:
        data_class_defs += "\n"
    data_class_helpers = _dart_data_class_helpers(data_classes)
    if data_class_helpers:
        data_class_helpers += "\n"

    return f"""// GENERATED by dart_cpp_bridge codegen — do not edit.
// Low-level impl (method ids + wire). Prefer importing api.dart singleton.
// ignore_for_file: unused_element

import 'dart:async';
import 'dart:typed_data';

import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';

{enums_s}
{data_class_defs}
{data_class_helpers}
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

    # Forwarders for opaque class methods (called by generated wrapper classes).
    for cls in ir.get("classes", []):
        if cls.get("kind") != "opaque_class":
            continue
        for method in cls.get("methods", []):
            impl_name = _class_impl_method_name(cls, method)
            params = []
            call_args = []
            is_instance = not method.get("is_static", False) and method["kind"] != "constructor"
            if is_instance:
                params.append("int handle")
                call_args.append("handle")
            for a in method["args"]:
                t = a["type"]
                if t.get("kind") == "stream_sink":
                    continue
                dn = _dart_param_name(a["name"])
                if t.get("kind") == "opaque_class":
                    params.append(f"int {dn}")
                elif t.get("kind") == "dart_fn":
                    arg_types = ", ".join(_dart_type(arg_t) for arg_t in t.get("args", []))
                    ret_t = _dart_type(t["return"])
                    params.append(f"FutureOr<{ret_t}> Function({arg_types}) {dn}")
                else:
                    params.append(f"{_dart_type(t)} {dn}")
                call_args.append(dn)
            param_s = ", ".join(params)
            call = ", ".join(call_args)

            if method["kind"] == "stream":
                sink_args = [a for a in method["args"] if a["type"].get("kind") == "stream_sink"]
                item_t = sink_args[0]["type"]["inner"]
                sig_ret = f"Stream<{_dart_type(item_t)}>"
            else:
                ret_type = method["return"]
                if method["kind"] == "constructor" or ret_type.get("kind") == "opaque_class":
                    ret_t = "int"
                else:
                    ret_t = _dart_type(ret_type)
                is_async = method["kind"] not in ("sync", "constructor")
                sig_ret = "Future<void>" if is_async and ret_t == "void" else (f"Future<{ret_t}>" if is_async else ret_t)

            async_keyword = "async " if method["kind"] not in ("sync", "constructor") and method["kind"] != "stream" else ""
            forwards.append(
                f"  {sig_ret} {impl_name}({param_s}) {async_keyword}=> _impl.{impl_name}({call});"
            )

    forwards_s = "\n".join(forwards)

    opaque_classes = [c for c in ir.get("classes", []) if c.get("kind") == "opaque_class"]
    opaque_names = [c["name"] for c in opaque_classes]
    hide_clause = f" hide {', '.join(opaque_names)}" if opaque_names else ""
    wrappers = _dart_opaque_class_wrappers(ir.get("classes", []))
    wrappers_s = f"\n\n{wrappers}\n" if wrappers else ""

    return f"""// GENERATED by dart_cpp_bridge codegen — do not edit.
// Public singleton facade (also used by top-level api_fn.dart).

import 'dart:async';

import 'package:dart_cpp_bridge/dart_cpp_bridge.dart'{hide_clause};

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
{wrappers_s}
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

import 'dart:async';

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
