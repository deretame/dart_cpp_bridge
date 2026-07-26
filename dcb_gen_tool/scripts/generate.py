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
from parse_api import parse_project, _stable_method_id  # noqa: E402


def _lower_first(s: str) -> str:
    if not s:
        return s
    return s[0].lower() + s[1:]


def _cap_first(s: str) -> str:
    if not s:
        return s
    return s[0].upper() + s[1:]


def _type_sig_fragment(t: dict[str, Any]) -> str:
    """Map a C++ IR type to a PascalCase fragment for factory constructor naming."""
    k = t.get("kind")
    mapping = {
        "i32": "Int32T",
        "u32": "Uint32T",
        "i64": "Int64T",
        "u64": "Uint64T",
        "f32": "Float",
        "f64": "Double",
        "bool": "Bool",
        "string": "String",
        "i128": "Int128",
        "u128": "Uint128",
    }
    if k in mapping:
        return mapping[k]
    if k == "enum":
        return t["name"]
    if k == "data_class":
        return t["name"]
    if k == "opaque_class":
        return t["name"]
    if k == "optional":
        return _type_sig_fragment(t["inner"])
    if k == "vector":
        return f"Vec{_type_sig_fragment(t['inner'])}"
    if k == "map":
        return f"Map{_type_sig_fragment(t['key'])}{_type_sig_fragment(t['value'])}"
    return "Unknown"


def _factory_ctor_name(class_name: str, args: list[dict[str, Any]]) -> str:
    """Generate Dart factory constructor name from C++ constructor args.

    No args → default factory (just class name).
    With args → ClassName.typeSig (e.g. Counter.int32T).
    """
    non_sink = [a for a in args if a["type"].get("kind") != "stream_sink"]
    if not non_sink:
        return class_name
    sig = "".join(_type_sig_fragment(a["type"]) for a in non_sink)
    return f"{class_name}.{_lower_first(sig)}"


def _is_optional_arg(a: dict[str, Any]) -> bool:
    """Check if an argument is optional (only std::optional<T> type).

    C++ default values do NOT make a parameter optional in Dart API.
    """
    return a["type"].get("kind") == "optional"


def _is_optional_sink_arg(a: dict[str, Any]) -> bool:
    """Check if arg is std::optional<StreamSink<T>> (optional progress sink)."""
    t = a["type"]
    return t.get("kind") == "optional" and t.get("inner", {}).get("kind") == "stream_sink"


def _dart_default_value(a: dict[str, Any]) -> str | None:
    """Return Dart default value literal from C++ default_value, or None."""
    dv = a.get("default_value")
    if dv is None:
        return None
    # C++ literals are mostly compatible with Dart for basic types.
    # Handle common cases: integers, floats, bool, string.
    k = a["type"].get("kind")
    if k == "bool":
        return "true" if dv in ("true", "1") else "false"
    if k == "string":
        # Ensure proper Dart string quoting.
        if dv.startswith('"') and dv.endswith('"'):
            return dv
        return f"'{dv}'"
    # Numeric types: use as-is (int/float literals are compatible).
    return dv


def _dart_named_params(args: list[dict[str, Any]], *, include_handle: bool = False) -> str:
    """Build Dart named parameter string with required/default/optional rules.

    - std::optional<T>: {T? name}
    - Has C++ default: {Type name = defaultValue}
    - std::optional<StreamSink<T>>: {StreamController<T>? name}
    - Otherwise: {required Type name}
    - No args: returns ''
    """
    parts: list[str] = []
    for a in args:
        t = a["type"]
        if t.get("kind") == "stream_sink":
            continue
        if _is_optional_sink_arg(a):
            # Optional sink → StreamController<T>? (always optional, no required).
            item_t = t["inner"]["inner"]
            dart_item_t = _dart_type(item_t)
            dn = _dart_param_name(a["name"])
            parts.append(f"StreamController<{dart_item_t}>? {dn}")
            continue
        dn = _dart_param_name(a["name"])
        if t.get("kind") == "dart_fn":
            arg_types = ", ".join(_dart_type(arg_t) for arg_t in t.get("args", []))
            ret_t = _dart_type(t["return"])
            type_s = f"FutureOr<{ret_t}> Function({arg_types})"
        elif t.get("kind") == "opaque_class":
            type_s = t["name"]
        else:
            type_s = _dart_type(t)

        if _is_optional_arg(a):
            # std::optional<T>: nullable without required
            if not type_s.endswith("?"):
                type_s = f"{type_s}?"
            parts.append(f"{type_s} {dn}")
        else:
            default_v = _dart_default_value(a)
            if default_v is not None:
                # Has C++ default value: use Dart default parameter
                parts.append(f"{type_s} {dn} = {default_v}")
            else:
                parts.append(f"required {type_s} {dn}")
    if not parts:
        return ""
    return "{" + ", ".join(parts) + "}"


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
    arg_parts = []
    for a in fn["args"]:
        if a["type"].get("kind") == "stream_sink":
            continue
        if _is_optional_sink_arg(a):
            arg_parts.append("std::move(sink)")
        else:
            arg_parts.append(a["name"])
    args = ", ".join(arg_parts)
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
    if k == "optional":
        inner = t["inner"]
        item = _cpp_write_item(inner, "v")
        return f"w.opt({expr}, [&](const auto& v) {{ {item} }});"
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
    if k == "vector":
        inner_type = _cpp_type(t["inner"])
        item_read = _cpp_read_item(t["inner"], reader)
        return f"{reader}.vec<{inner_type}>([&]() {{ return {item_read}; }})"
    if k == "array":
        inner_type = _cpp_type(t["inner"])
        size = t["size"]
        item_read = _cpp_read_item(t["inner"], reader)
        return f"{reader}.arr<{inner_type}, {size}>([&]() {{ return {item_read}; }})"
    if k == "set":
        inner_type = _cpp_type(t["inner"])
        item_read = _cpp_read_item(t["inner"], reader)
        return f"{reader}.set<{inner_type}>([&]() {{ return {item_read}; }})"
    if k == "optional":
        inner_type = _cpp_type(t["inner"])
        item_read = _cpp_read_item(t["inner"], reader)
        return f"{reader}.opt<{inner_type}>([&]() {{ return {item_read}; }})"
    if k == "map":
        key_type = _cpp_type(t["key"])
        val_type = _cpp_type(t["value"])
        key_read = _cpp_read_item(t["key"], reader)
        val_read = _cpp_read_item(t["value"], reader)
        return (
            f"{reader}.map<{key_type}, {val_type}>("
            f"[&]() {{ return {key_read}; }}, "
            f"[&]() {{ return {val_read}; }})"
        )
    if k == "i128":
        return f"{reader}.i128()"
    if k == "u128":
        return f"{reader}.u128()"
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
            sync_arg_reads = "\n        ".join(_cpp_read_arg(a, sync=True) for a in non_sink_args)

            if is_constructor:
                ctor_call = f"std::make_shared<{class_q}>({', '.join(arg_names)})"
                counter_var = f"g_{class_name}_alive_count"
                body = f"""
      case {mid}: {{
        ByteReader r(frame.payload.data(), frame.payload.size());
        {arg_reads}
        auto obj = {ctor_call};
        {counter_var}.increment(session_id);
        const auto handle = dcb::ObjectHandleRegistry::instance().insert(session_id, obj, [session_id](std::shared_ptr<void>&) {{
          {counter_var}.decrement(session_id);
        }});
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
    {counter_var}.increment(session_id);
    const auto handle = dcb::ObjectHandleRegistry::instance().insert(session_id, obj, [session_id](std::shared_ptr<void>&) {{
      {counter_var}.decrement(session_id);
    }});
    ByteWriter w;
    w.u64(handle);
    return make_frame(MsgType::kResponseOk, frame.request_id, frame.method_id, w.raw());
  }}"""
                cases.append(body)
                sync_cases.append(sync_body)
                continue

            err_msg = f"{class_name} handle not found or already dropped"
            fn_label = f"{class_name}::{m['name']}"
            if is_static:
                handle_block = arg_reads
                sync_handle_block = sync_arg_reads
                # Special case: aliveCount() reads the generated counter.
                if m["name"] == "aliveCount" and not arg_names:
                    counter_var = f"g_{class_name}_alive_count"
                    call = f"{counter_var}.load(session_id)"
                else:
                    call = f"{class_q}::{m['name']}({', '.join(arg_names)})"
            else:
                handle_block = f"""const auto handle = r.u64();
        auto obj = dcb::ObjectHandleRegistry::instance().get(handle);
        if (!obj) {{
          post_err(session, gen, req, method, "{fn_label}", "{err_msg}");
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
        {sync_arg_reads}"""
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
                fn_label = f"{class_name}::{m['name']}"
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
                post_err(session, gen, req, method, "{fn_label}", e.what());
              }} catch (...) {{
                post_err(session, gen, req, method, "{fn_label}", "unknown");
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
                fn_label = f"{class_name}::{m['name']}"
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
              post_err(session, gen, req, method, "{fn_label}", msg);
            }});
          }} catch (...) {{
            asio::post(*io, [session, gen, req, method]() {{
              post_err(session, gen, req, method, "{fn_label}", "unknown");
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


def _cpp_read_arg(a: dict[str, Any], *, sync: bool = False) -> str:
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
        err_msg = f"{t['name']} handle not found or already dropped"
        if sync:
            return (
                f"const auto {name}Handle = r.u64();\n"
                f"    auto {name}Obj = dcb::ObjectHandleRegistry::instance().get({name}Handle);\n"
                f"    if (!{name}Obj) {{\n"
                f"      ByteWriter ew; ew.i32(1); ew.str(\"{err_msg}\");\n"
                f"      return make_frame(MsgType::kResponseErr, frame.request_id, frame.method_id, ew.raw());\n"
                f"    }}\n"
                f"    {q}& {name} = *static_cast<{q}*>({name}Obj.get());"
            )
        return (
            f"const auto {name}Handle = r.u64();\n"
            f"        auto {name}Obj = dcb::ObjectHandleRegistry::instance().get({name}Handle);\n"
            f"        if (!{name}Obj) {{\n"
            f"          post_err(session, gen, req, method, \"dispatch\", \"{err_msg}\");\n"
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
        counter_var = f"g_{t['name']}_alive_count"
        return (
            f"{{ auto __obj = std::make_shared<{q}>(std::move({expr})); "
            f"{counter_var}.increment(session_id); "
            f"const auto __handle = dcb::ObjectHandleRegistry::instance().insert("
            f"session_id, __obj, [session_id](std::shared_ptr<void>&) {{ "
            f"{counter_var}.decrement(session_id); }}); "
            f"w.u64(__handle); }}"
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
        if _is_optional_sink_arg(a):
            continue
        n = a["dart_name"]

        if k == "dart_fn":
            lines.append(f"_payload.u64(_{a['dart_name']}Id);")
        elif k == "opaque_class":
            lines.append(f"_payload.u64({n}.handle);")
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
  {class_name}.fromHandle({{required super.bridge, required super.handle}});

{ctor_s}

{method_s}
}}"""
        )

    return "\n\n".join(wrappers)


def generate_cpp(ir: dict[str, Any], api_includes: list[str]) -> tuple[str, str]:
    """Returns (hpp, cpp)."""
    fns = ir["functions"]
    data_classes = [c for c in ir.get("classes", []) if c.get("kind") == "data_class"]
    opaque_classes = [c for c in ir.get("classes", []) if c.get("kind") == "opaque_class"]
    cpp_helpers = _cpp_data_class_helpers(data_classes)
    # Generate per-class alive instance counters (per-session) for opaque classes.
    alive_counters = "\n".join(
        f"struct AliveCounter_{c['name']} {{\n"
        f"  std::mutex mu;\n"
        f"  std::unordered_map<std::uint64_t, std::int32_t> counts;\n"
        f"  void increment(std::uint64_t sid) {{ std::lock_guard<std::mutex> lk(mu); counts[sid]++; }}\n"
        f"  void decrement(std::uint64_t sid) {{ std::lock_guard<std::mutex> lk(mu); auto it = counts.find(sid); if (it != counts.end() && --it->second <= 0) counts.erase(it); }}\n"
        f"  std::int32_t load(std::uint64_t sid) {{ std::lock_guard<std::mutex> lk(mu); auto it = counts.find(sid); return it != counts.end() ? it->second : 0; }}\n"
        f"}};\n"
        f"static AliveCounter_{c['name']} g_{c['name']}_alive_count;"
        for c in opaque_classes
    )
    if alive_counters:
        alive_counters = (
            "// Per-class alive instance counters (per-session, generated for opaque classes).\n"
            + alive_counters
        )
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
        # Optional sink args are handled separately (not read as normal args).
        opt_sink_arg = next((a for a in fn["args"] if _is_optional_sink_arg(a)), None)
        reads = "\n        ".join(
            _cpp_read_arg(a) for a in fn["args"]
            if a["type"].get("kind") != "stream_sink" and not _is_optional_sink_arg(a)
        )
        sync_reads = "\n    ".join(
            _cpp_read_arg(a, sync=True) for a in fn["args"]
            if a["type"].get("kind") != "stream_sink" and not _is_optional_sink_arg(a)
        )
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
    {sync_reads}
    ByteWriter w;
    {{
      auto out = {call};
      {write}
    }}
    return make_frame(MsgType::kResponseOk, frame.request_id, frame.method_id, w.raw());
  }}"""
            sync_cases.append(sync_body)

        elif kind == "async":
            move_caps = ", ".join(
                f"{a['name']} = std::move({a['name']})"
                if a["type"].get("kind") == "string"
                else a["name"]
                for a in fn["args"]
                if a["type"].get("kind") != "stream_sink" and not _is_optional_sink_arg(a)
            )
            if move_caps:
                move_caps = ", " + move_caps

            # Optional sink setup (read stream_id, create sink if non-zero).
            sink_setup = ""
            sink_capture = ""
            if opt_sink_arg:
                sink_inner = opt_sink_arg["type"]["inner"]["inner"]
                sink_encode = _cpp_write_item(sink_inner, "v")
                sink_setup = f"""
        const auto _stream_id = r.u64();
        std::optional<dcb::StreamSink<{_cpp_type(sink_inner)}>> sink;
        if (_stream_id != 0) {{
          sink.emplace(session.get(), _stream_id, gen, method, []({_cpp_type(sink_inner)} v) {{
            ByteWriter w;
            {sink_encode}
            return w.raw();
          }});
        }}"""
                sink_capture = ", sink = std::move(sink)"

            ret_kind = fn["return"].get("kind")
            if ret_kind == "void":
                call_stmt = f"co_await {call};"
            else:
                call_stmt = f"auto out = co_await {call};"

            body = f"""
      case {mid}: {{
        ByteReader r(frame.payload.data(), frame.payload.size());
        {reads}{sink_setup}
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method{move_caps}{sink_capture}]() -> async_simple::coro::Lazy<> {{
              try {{
                {call_stmt}
                ByteWriter w;
                {write}
                post_ok(session, gen, req, method, w.raw());
              }} catch (const std::exception& e) {{
                post_err(session, gen, req, method, "{fn['name']}", e.what());
              }} catch (...) {{
                post_err(session, gen, req, method, "{fn['name']}", "unknown");
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
                if a["type"].get("kind") != "stream_sink" and not _is_optional_sink_arg(a)
            )
            if move_caps:
                lambda_extra = ", " + move_caps
            else:
                lambda_extra = ""

            # Optional sink setup for normal (thread pool) functions.
            sink_setup = ""
            sink_capture = ""
            if opt_sink_arg:
                sink_inner = opt_sink_arg["type"]["inner"]["inner"]
                sink_encode = _cpp_write_item(sink_inner, "v")
                sink_setup = f"""
        const auto _stream_id = r.u64();
        std::optional<dcb::StreamSink<{_cpp_type(sink_inner)}>> sink;
        if (_stream_id != 0) {{
          sink.emplace(session.get(), _stream_id, gen, method, []({_cpp_type(sink_inner)} v) {{
            ByteWriter w;
            {sink_encode}
            return w.raw();
          }});
        }}"""
                sink_capture = ", sink = std::move(sink)"

            ret_kind = fn["return"].get("kind")
            if ret_kind == "void":
                call_stmt = f"{call};"
                post_block = "post_ok(session, gen, req, method, {});"
            else:
                call_stmt = f"auto out = {call};"
                post_block = f"""asio::post(*io, [session, gen, req, method, out = std::move(out)]() {{
              ByteWriter w;
              {write}
              post_ok(session, gen, req, method, w.raw());
            }});"""

            body = f"""
      case {mid}: {{
        ByteReader r(frame.payload.data(), frame.payload.size());
        {reads}{sink_setup}
        auto* io = &Runtime::instance().io();
        asio::post(Runtime::instance().pool(), [session, gen, req, method, io{lambda_extra}{sink_capture}]() {{
          try {{
            {call_stmt}
            {post_block}
          }} catch (const std::exception& e) {{
            asio::post(*io, [session, gen, req, method, msg = std::string(e.what())]() {{
              post_err(session, gen, req, method, "{fn['name']}", msg);
            }});
          }} catch (...) {{
            asio::post(*io, [session, gen, req, method]() {{
              post_err(session, gen, req, method, "{fn['name']}", "unknown");
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
#include "dart_cpp_bridge/dispatch.hpp"
#include "dart_cpp_bridge/error_config.hpp"
#include "dart_cpp_bridge/object_handle.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

{inc_lines}

#include <async_simple/coro/Lazy.h>

#include <asio/post.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
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
              std::uint32_t method, const char* fn, const std::string& msg) {{
  ByteWriter w;
  w.i32(1);
  w.str(dcb::error::format(fn, msg));
  s->try_post(gen, make_frame(MsgType::kResponseErr, req, method, w.raw()));
}}

}}  // namespace

{alive_counters}

{cpp_helpers}

void dispatch_request(std::shared_ptr<Session> session, std::uint64_t session_id,
                      const std::uint8_t* data, std::size_t len) {{
  const auto gen = session->generation();
  FrameHeader frame;
  try {{
    frame = parse_frame(data, len);
  }} catch (const std::exception& e) {{
    post_err(session, gen, 0, 0, "dispatch", std::string("bad frame: ") + e.what());
    return;
  }} catch (...) {{
    post_err(session, gen, 0, 0, "dispatch", "bad frame");
    return;
  }}

  const auto req = frame.request_id;
  const auto method = frame.method_id;

  try {{
    switch (method) {{
{cases_s}
      default:
        post_err(session, gen, req, method, "dispatch", "unknown method");
        break;
    }}
  }} catch (const std::exception& e) {{
    post_err(session, gen, req, method, "dispatch", e.what());
  }} catch (...) {{
    post_err(session, gen, req, method, "dispatch", "unknown");
  }}
}}

std::vector<std::uint8_t> dispatch_sync(std::uint64_t session_id, const std::uint8_t* data, std::size_t len) {{
  auto frame = parse_frame(data, len);
{sync_s}
  throw std::runtime_error("sync: method not sync-capable");
}}

}}  // namespace demo
}}  // namespace dcb

// Auto-register dispatch at DLL load time.
namespace {{
const bool _dcb_registered = [] {{
  dcb::set_dispatch(&dcb::demo::dispatch_request, &dcb::demo::dispatch_sync);
  return true;
}}();
}}  // namespace
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
        ensure_alive_lines: list[str] = []
        opt_sink_info = None  # {"dart_name": ..., "item_t": ..., "dart_item_type": ...}
        for a in args:
            if a["type"].get("kind") == "stream_sink":
                continue
            if _is_optional_sink_arg(a):
                # Optional sink → StreamController<T>? parameter.
                item_t = a["type"]["inner"]["inner"]
                dart_item_t = _dart_type(item_t)
                dn = a["dart_name"]
                params.append(f"StreamController<{dart_item_t}>? {dn}")
                call_args.append(dn)
                opt_sink_info = {
                    "dart_name": dn,
                    "item_t": item_t,
                    "dart_item_type": dart_item_t,
                    "decode_expr": _dart_read_item(item_t, "_r"),
                }
                continue
            if a["type"].get("kind") == "opaque_class":
                # Opaque params use the class name, not raw int.
                cls_name = a["type"]["name"]
                params.append(f"{cls_name} {a['dart_name']}")
                call_args.append(a["dart_name"])
                ensure_alive_lines.append(f"{a['dart_name']}.ensureAlive();")
                continue
            type_s = _dart_type(a['type'])
            # std::optional<T> args become nullable at wire level.
            if _is_optional_arg(a) and not type_s.endswith('?'):
                type_s = f"{type_s}?"
            params.append(f"{type_s} {a['dart_name']}")
            call_args.append(a["dart_name"])
        # Return type: opaque_class returns wrapped object.
        fn_ret = fn["return"]
        if fn_ret.get("kind") == "opaque_class":
            ret_t = fn_ret["name"]
            read_ret = f"{ret_t}.fromHandle(bridge: bridge, handle: ByteReader(_bytes).u64())"
        else:
            ret_t = _dart_type(fn_ret)
            read_ret = _dart_read_ret(fn_ret, "_bytes")
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
            "ensure_alive_lines": ensure_alive_lines,
            "payload_lines": _dart_payload_lines(args),
            "dart_fn_args": dart_fn_args,
            "read_ret": read_ret,
            "stream_decode_expr": stream_decode_expr,
            "opt_sink_info": opt_sink_info,
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


def generate_dart_impl(ir: dict[str, Any], impl_class: str = "BridgeApiImpl", api_subdir: str = "api") -> str:
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
        ensure_alive_lines = m.get("ensure_alive_lines", [])

        # Build payload body. DartFn callbacks must be registered before the
        # payload is sent and unregistered after the C++ call returns.
        body_lines: list[str] = []
        # ensureAlive() checks for opaque params go first.
        body_lines.extend(ensure_alive_lines)
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
        elif m.get("opt_sink_info"):
            # Async method with optional StreamSink (progress events + result).
            osi = m["opt_sink_info"]
            for line in payload_lines:
                body_lines.append(f"{indent}{line}")
            decode_item = osi["decode_expr"]
            dart_item_t = osi["dart_item_type"]
            ctrl_name = osi["dart_name"]
            if ret_t == "void":
                body_lines.append(
                    f"{indent}await bridge.invokeAsyncMethodWithStream<{dart_item_t}>("
                    f"{dart_name}Id, _payload, {ctrl_name}, "
                    f"(final _r) => {decode_item});"
                )
            else:
                body_lines.append(
                    f"{indent}final _bytes = await bridge.invokeAsyncMethodWithStream<{dart_item_t}>("
                    f"{dart_name}Id, _payload, {ctrl_name}, "
                    f"(final _r) => {decode_item});"
                )
                body_lines.append(f"{indent}return {read_ret};")
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
            ensure_alive_lines_cls: list[str] = []
            if is_instance:
                params.append(f"{cls['name']} self")
                call_args.append("self")
                ensure_alive_lines_cls.append("self.ensureAlive();")
            for a in method["args"]:
                t = a["type"]
                if t.get("kind") == "stream_sink":
                    continue
                dart_name = a["dart_name"]
                if t.get("kind") == "opaque_class":
                    params.append(f"{t['name']} {dart_name}")
                    ensure_alive_lines_cls.append(f"{dart_name}.ensureAlive();")
                elif t.get("kind") == "dart_fn":
                    arg_types = ", ".join(_dart_type(arg_t) for arg_t in t.get("args", []))
                    ret_t = _dart_type(t["return"])
                    params.append(f"FutureOr<{ret_t}> Function({arg_types}) {dart_name}")
                else:
                    type_s = _dart_type(t)
                    if _is_optional_arg(a) and not type_s.endswith('?'):
                        type_s = f"{type_s}?"
                    params.append(f"{type_s} {dart_name}")
                call_args.append(dart_name)
            param_s = ", ".join(params)

            # Payload construction (handle first for instance methods).
            payload_lines = ["final _payload = ByteWriter();"]
            if is_instance:
                payload_lines.append("_payload.u64(self.handle);")
            for a in method["args"]:
                t = a["type"]
                n = a["dart_name"]
                k = t.get("kind")
                if k == "stream_sink":
                    continue
                if k == "dart_fn":
                    payload_lines.append(f"_payload.u64(_{a['dart_name']}Id);")
                elif k == "opaque_class":
                    payload_lines.append(f"_payload.u64({n}.handle);")
                else:
                    payload_lines.extend(_dart_write_item(t, n))

            # Determine return type handling.
            ret_type = method["return"]
            if is_constructor:
                ret_t = cls["name"]
                read_ret = f"{cls['name']}.fromHandle(bridge: bridge, handle: ByteReader(_bytes).u64())"
            elif ret_type.get("kind") == "opaque_class":
                ret_t = ret_type["name"]
                read_ret = f"{ret_t}.fromHandle(bridge: bridge, handle: ByteReader(_bytes).u64())"
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
            # ensureAlive() checks for self and opaque params go first.
            body_lines.extend(ensure_alive_lines_cls)
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

    # Opaque class wrappers live in gcm_generated alongside BridgeApiImpl.
    # Collect API file imports for opaque classes (impl references their types).
    opaque_api_imports: list[str] = []
    if opaque_classes:
        from pathlib import PurePosixPath, PureWindowsPath
        seen_headers: set[str] = set()
        for cls in opaque_classes:
            h = cls.get("header", "")
            if h and h not in seen_headers:
                seen_headers.add(h)
                hp = PureWindowsPath(h) if "\\" in h else PurePosixPath(h)
                dart_fname = hp.stem + ".dart"
                opaque_api_imports.append(f"import '{api_subdir}/{dart_fname}';")
    opaque_imports_s = "\n".join(opaque_api_imports)
    if opaque_imports_s:
        opaque_imports_s += "\n"

    return f"""// GENERATED by dart_cpp_bridge codegen — do not edit.
// ignore_for_file: unused_element, unused_import

import 'dart:async';
import 'dart:typed_data';

import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';

{opaque_imports_s}{enums_s}
{data_class_defs}
{data_class_helpers}
/// Wire-level API singleton. Access via [BridgeApiImpl.instance].
final class {impl_class} {{
  {impl_class}._(this.bridge);

  final DartCppBridge bridge;

  // ── Singleton ──
  static {impl_class}? _instance;
  static {impl_class} get instance {{
    final i = _instance;
    if (i == null) throw StateError('DcbLib.init() must be called first');
    return i;
  }}
  static void initSingleton(DartCppBridge bridge) => _instance = {impl_class}._(bridge);
  static void disposeSingleton() => _instance = null;

{ids_s}

{methods_s}
}}
"""


def generate_dart_init(
    *,
    lib_class: str = "DcbLib",
    impl_class: str = "BridgeApiImpl",
    impl_import: str = "../gcm_generated.dart",
) -> str:
    """Generate api/init.dart — DcbLib management class."""
    return f"""// GENERATED by dart_cpp_bridge codegen — do not edit.

import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';

import '{impl_import}';

/// dart_cpp_bridge management class.
///
/// Usage:
/// ```dart
/// await {lib_class}.init(libraryPath: '...');
/// final v = bridgeVersion();
/// {lib_class}.shutdown(); // only on process exit
/// ```
final class {lib_class} {{
  {lib_class}._();

  static DartCppBridge? _bridge;

  /// Whether the bridge has been initialized.
  static bool get isInitialized => _bridge != null;

  /// Underlying bridge instance (available after init).
  static DartCppBridge get bridge {{
    final b = _bridge;
    if (b == null) throw StateError('{lib_class}.init() must be called first');
    return b;
  }}

  /// Initialize (call once per Isolate).
  ///
  /// [threadPoolSize] sets the native thread pool concurrency (default 4).
  /// [verboseErrors] controls whether C++ errors include function names (default true).
  static Future<void> init({{String? libraryPath, int threadPoolSize = 4, bool verboseErrors = true}}) async {{
    if (_bridge != null) return;
    final b = await DartCppBridge.init(libraryPath: libraryPath, poolThreads: threadPoolSize);
    b.setVerboseErrors(verboseErrors);
    _bridge = b;
    {impl_class}.initSingleton(b);
  }}

  /// Close this Isolate's session (optional; Finalizer also closes).
  static void dispose() {{
    _bridge?.dispose();
    _bridge = null;
    {impl_class}.disposeSingleton();
  }}

  /// Process-wide shutdown (main Isolate / app exit only).
  static void shutdown() {{
    _bridge?.shutdown();
    _bridge = null;
    {impl_class}.disposeSingleton();
  }}

  /// Enable or disable verbose error messages (default: enabled).
  ///
  /// When enabled, C++ error messages are prefixed with `[function_name] `.
  static void setVerboseErrors(bool enabled) {{
    bridge.setVerboseErrors(enabled);
  }}
}}
"""


def _dart_api_fn_signature(
    dart_name: str,
    args: list[dict[str, Any]],
    sig_ret: str,
) -> str:
    """Build a top-level function signature with named params."""
    param_s = _dart_named_params(args)
    return f"{sig_ret} {dart_name}({param_s})"


def _dart_call_args_positional(args: list[dict[str, Any]]) -> str:
    """Build positional call args for forwarding to BridgeApiImpl."""
    parts: list[str] = []
    for a in args:
        t = a["type"]
        if t.get("kind") == "stream_sink":
            continue
        dn = _dart_param_name(a["name"])
        parts.append(dn)
    return ", ".join(parts)


def generate_dart_api_files(
    ir: dict[str, Any],
    *,
    impl_class: str = "BridgeApiImpl",
    lib_class: str = "DcbLib",
) -> dict[str, str]:
    """Generate per-header Dart API files.

    Returns a dict: {filename: content} where filename is like 'bridge_api.dart'.
    """
    from pathlib import PurePosixPath, PureWindowsPath

    # Group functions by header file.
    header_fns: dict[str, list] = {}
    for fn in ir["functions"]:
        h = fn.get("header", "")
        header_fns.setdefault(h, []).append(fn)

    # Group classes by header file.
    header_classes: dict[str, list] = {}
    for cls in ir.get("classes", []):
        h = cls.get("header", "")
        header_classes.setdefault(h, []).append(cls)

    # Group enums by header file.
    header_enums: dict[str, list] = {}
    for e in ir.get("enums", []):
        h = e.get("header", "")
        header_enums.setdefault(h, []).append(e)

    # Collect all headers.
    all_headers = set(header_fns.keys()) | set(header_classes.keys()) | set(header_enums.keys())

    result: dict[str, str] = {}
    for header in sorted(all_headers):
        if not header:
            continue
        # Derive filename: bridge_api.h -> bridge_api.dart
        hp = PureWindowsPath(header) if "\\" in header else PurePosixPath(header)
        dart_filename = hp.stem + ".dart"

        fns = header_fns.get(header, [])
        classes = header_classes.get(header, [])
        enums = header_enums.get(header, [])

        # Determine what types to export from gcm_generated.dart
        export_names: list[str] = []
        for e in enums:
            export_names.append(e["name"])
        for cls in classes:
            if cls.get("kind") == "data_class":
                export_names.append(cls["name"])

        has_opaque = any(c.get("kind") == "opaque_class" for c in classes)
        # Also check free functions for opaque params/returns.
        if not has_opaque:
            has_opaque = any(
                a["type"].get("kind") == "opaque_class"
                for fn in fns for a in fn["args"]
            ) or any(
                fn["return"].get("kind") == "opaque_class" for fn in fns
            )
        has_dart_fn = any(
            a["type"].get("kind") == "dart_fn"
            for fn in fns for a in fn["args"]
        ) or any(
            a["type"].get("kind") == "dart_fn"
            for cls in classes for m in cls.get("methods", []) for a in m["args"]
        )
        has_opt_sink = any(
            _is_optional_sink_arg(a)
            for fn in fns for a in fn["args"]
        )

        lines: list[str] = []
        lines.append("// GENERATED by dart_cpp_bridge codegen — do not edit.")
        lines.append(f"// Source: {hp.name}")
        lines.append("")
        if has_dart_fn or has_opt_sink:
            lines.append("import 'dart:async';")
            lines.append("")
        if has_opaque:
            lines.append("import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';")
            lines.append("")
        lines.append("import '../gcm_generated.dart';")
        lines.append("")
        if export_names:
            lines.append(f"export '../gcm_generated.dart' show {', '.join(sorted(export_names))};")
            lines.append("")

        # Generate top-level functions — all are thin `=>` forwards.
        if fns:
            lines.append("// " + "═" * 45)
            lines.append("// Functions")
            lines.append("// " + "═" * 45)
            lines.append("")
            for fn in fns:
                dart_name = _dart_fn_name(fn["name"])
                args = [{**a, "dart_name": _dart_param_name(a["name"])} for a in fn["args"]]
                is_stream = fn["kind"] == "stream"
                is_async = not is_stream and fn["kind"] != "sync"
                ret = fn["return"]

                if is_stream:
                    sink_args = [a for a in args if a["type"].get("kind") == "stream_sink"]
                    item_t = sink_args[0]["type"]["inner"]
                    sig_ret = f"Stream<{_dart_type(item_t)}>"
                elif ret.get("kind") == "opaque_class":
                    sig_ret = f"Future<{ret['name']}>" if is_async else ret["name"]
                else:
                    ret_t = _dart_type(ret)
                    if is_async:
                        sig_ret = "Future<void>" if ret_t == "void" else f"Future<{ret_t}>"
                    else:
                        sig_ret = ret_t

                param_s = _dart_named_params(args)
                call_args = _dart_call_args_positional(args)
                call = f"{impl_class}.instance.{dart_name}({call_args})"
                lines.append(f"{sig_ret} {dart_name}({param_s}) => {call};")
                lines.append("")

        # Generate opaque class wrappers (thin forwarding methods).
        opaque_classes = [c for c in classes if c.get("kind") == "opaque_class"]
        for cls in opaque_classes:
            lines.append("// " + "═" * 45)
            lines.append(f"// {cls['name']}")
            lines.append("// " + "═" * 45)
            lines.append("")
            lines.append(_dart_opaque_class_wrapper_new(cls, impl_class=impl_class, lib_class=lib_class))
            lines.append("")

        result[dart_filename] = "\n".join(lines)
    return result


def _dart_opaque_class_wrapper_new(
    cls: dict[str, Any],
    *,
    impl_class: str = "BridgeApiImpl",
    lib_class: str = "DcbLib",
) -> str:
    """Generate a single opaque class wrapper with thin forwarding methods.

    All lifecycle checks (ensureAlive) and handle extraction are done inside
    BridgeApiImpl; the class methods are simple `=>` forwards.
    """
    class_name = cls["name"]
    lines: list[str] = []
    lines.append(f"/// Opaque wrapper for `{cls['qualified']}`.")
    lines.append(f"final class {class_name} extends CppOpaqueInterface {{")
    lines.append(f"  {class_name}.fromHandle({{required super.bridge, required super.handle}});")
    lines.append("")

    # Constructors as factory — impl returns the wrapped object directly.
    ctors = [m for m in cls.get("methods", []) if m["kind"] == "constructor"]
    if ctors:
        lines.append("  // ── Constructors ──")
        lines.append("")
        for method in ctors:
            factory_name = _factory_ctor_name(class_name, method["args"])
            impl_name = _class_impl_method_name(cls, method)
            param_s = _dart_named_params(method["args"])
            call_args = _dart_call_args_positional(method["args"])
            lines.append(f"  factory {factory_name}({param_s}) =>")
            lines.append(f"      {impl_class}.instance.{impl_name}({call_args});")
            lines.append("")

    # Instance methods.
    instance_methods = [
        m for m in cls.get("methods", [])
        if m["kind"] != "constructor" and not m.get("is_static", False)
    ]
    if instance_methods:
        lines.append("  // ── Instance Methods ──")
        lines.append("")
        for method in instance_methods:
            lines.extend(_dart_opaque_method_lines(cls, method, is_static=False,
                                                   impl_class=impl_class, lib_class=lib_class))
            lines.append("")

    # Static methods.
    static_methods = [
        m for m in cls.get("methods", [])
        if m.get("is_static", False)
    ]
    if static_methods:
        lines.append("  // ── Static Methods ──")
        lines.append("")
        for method in static_methods:
            lines.extend(_dart_opaque_method_lines(cls, method, is_static=True,
                                                   impl_class=impl_class, lib_class=lib_class))
            lines.append("")

    lines.append("}")
    return "\n".join(lines)


def _dart_opaque_method_lines(
    cls: dict[str, Any],
    method: dict[str, Any],
    *,
    is_static: bool,
    impl_class: str = "BridgeApiImpl",
    lib_class: str = "DcbLib",
) -> list[str]:
    """Generate thin forwarding lines for an opaque class method.

    All ensureAlive/handle logic lives in BridgeApiImpl; the class method is
    a simple `=>` expression passing `this` (for instance methods) and args.
    """
    is_stream = method["kind"] == "stream"
    is_async = method["kind"] not in ("sync", "constructor")
    dart_name = _dart_fn_name(method["name"])
    ret = method["return"]

    if is_stream:
        sink_arg = next(a for a in method["args"] if a["type"].get("kind") == "stream_sink")
        item_t = sink_arg["type"]["inner"]
        sig_ret = f"Stream<{_dart_type(item_t)}>"
    elif ret.get("kind") == "opaque_class":
        sig_ret = f"Future<{ret['name']}>" if is_async else ret["name"]
    else:
        ret_t = _dart_type(ret)
        if is_async:
            sig_ret = "Future<void>" if ret_t == "void" else f"Future<{ret_t}>"
        else:
            sig_ret = ret_t

    param_s = _dart_named_params(method["args"])
    impl_name = _class_impl_method_name(cls, method)

    # Build call args: `this` first for instance methods, then named params.
    call_parts: list[str] = []
    if not is_static:
        call_parts.append("this")
    for a in method["args"]:
        t = a["type"]
        if t.get("kind") == "stream_sink":
            continue
        dn = _dart_param_name(a["name"])
        call_parts.append(dn)
    call_args = ", ".join(call_parts)
    call = f"{impl_class}.instance.{impl_name}({call_args})"

    result: list[str] = []
    static_kw = "static " if is_static else ""
    result.append(f"  {static_kw}{sig_ret} {dart_name}({param_s}) => {call};")
    return result


def run_generate(config_path: Path) -> dict[str, Any]:
    result = parse_project(config_path)
    cfg = result["cfg"]
    ir = result["ir"]
    raw = cfg.get("raw") or {}

    impl_class = str(raw.get("dart_impl_class", "BridgeApiImpl"))
    lib_class = str(raw.get("dart_lib_class", "DcbLib"))
    impl_file = str(raw.get("dart_impl_file", "gcm_generated.dart"))
    api_subdir = str(raw.get("dart_api_subdir", "api"))

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
    dart_impl = generate_dart_impl(ir, impl_class=impl_class, api_subdir=api_subdir)
    dart_init = generate_dart_init(
        lib_class=lib_class,
        impl_class=impl_class,
        impl_import=f"../{impl_file}",
    )
    api_files = generate_dart_api_files(
        ir,
        impl_class=impl_class,
        lib_class=lib_class,
    )

    cpp_out: Path = cfg["cpp_wire_output"]
    dart_out: Path = cfg["dart_output"]
    api_out: Path = dart_out / api_subdir
    cpp_out.mkdir(parents=True, exist_ok=True)
    dart_out.mkdir(parents=True, exist_ok=True)
    api_out.mkdir(parents=True, exist_ok=True)

    (cpp_out / "wire_dispatch.hpp").write_text(hpp, encoding="utf-8")
    (cpp_out / "wire_dispatch.cpp").write_text(cpp, encoding="utf-8")
    (cpp_out / "ir.json").write_text(json.dumps(ir, indent=2) + "\n", encoding="utf-8")
    (dart_out / impl_file).write_text(dart_impl, encoding="utf-8")
    (api_out / "init.dart").write_text(dart_init, encoding="utf-8")
    for filename, content in api_files.items():
        (api_out / filename).write_text(content, encoding="utf-8")

    return {
        "functions": len(ir["functions"]),
        "cpp_out": str(cpp_out),
        "dart_out": str(dart_out),
        "dart_impl": impl_file,
        "api_subdir": api_subdir,
        "api_files": list(api_files.keys()),
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
    print(f"  Dart init   -> {info['dart_out']}/{info['api_subdir']}/init.dart")
    for f in info["api_files"]:
        print(f"  Dart api    -> {info['dart_out']}/{info['api_subdir']}/{f}")
    for fn in info["ir"]["functions"]:
        print(f"  - {fn['kind']:6} id={fn['method_id']} {fn['qualified']}")
    return 0 if info["functions"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
