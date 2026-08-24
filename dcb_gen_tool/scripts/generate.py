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
from parse_api import parse_project, _dart_identifier, _stable_method_id  # noqa: E402


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
        "time_point": "DateTime",
        "u8_ptr": "U8Ptr",
        "u8": "Uint8T",
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
            type_s = f"Future<{ret_t}> Function({arg_types})"
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
    if k == "vector" and t["inner"].get("kind") == "u8":
        return "Uint8List"
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
        return f"Future<{ret}> Function({arg_types})"
    if k in ("pair", "tuple"):
        elems = ", ".join(_dart_type(e) for e in t["elements"])
        return f"({elems})"
    if k == "data_class":
        return t["name"]
    if k == "opaque_class":
        return "int"
    if k == "time_point":
        return "DateTime"
    if k == "u8_ptr":
        # Dart FFI names the 8-bit unsigned type `Uint8` (not `UInt8`).
        return "Pointer<Uint8>"
    return {
        "i32": "int",
        "u32": "int",
        "u8": "int",
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
    if k == "u8":
        return "std::uint8_t"
    if k == "i64":
        return "std::int64_t"
    if k == "bool":
        return "bool"
    if k == "string":
        return "std::string"
    if k == "void":
        # 仅用于 DartFn 的返回类型（如 dcb::DartFn<void(int64_t, int64_t)>），
        # 函数本身的 void 返回在写回包处单独处理，不经此分支。
        return "void"
    if k == "dart_fn":
        # e.g. signature "std::string (std::string)" →
        # dcb::DartFn<std::string (std::string)>
        return f"dcb::DartFn<{t['signature']}>"
    if k == "enum":
        q = t["qualified"]
        if not q.startswith("::"):
            q = "::" + q
        return q
    if k == "optional":
        return f"std::optional<{_cpp_type(t['inner'])}>"
    if k == "stream_sink":
        return f"dcb::StreamSink<{_cpp_type(t['inner'])}>"
    if k == "vector":
        return f"std::vector<{_cpp_type(t['inner'])}>"
    if k == "array":
        return f"std::array<{_cpp_type(t['inner'])}, {t['size']}>"
    if k == "set":
        if t.get("ordered"):
            return f"std::set<{_cpp_type(t['inner'])}>"
        return f"std::unordered_set<{_cpp_type(t['inner'])}>"
    if k == "map":
        if t.get("ordered"):
            return f"std::map<{_cpp_type(t['key'])}, {_cpp_type(t['value'])}>"
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
    if k == "time_point":
        return "std::chrono::system_clock::time_point"
    if k == "u8_ptr":
        return "std::uint8_t*"
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


def _opaque_type_qualified(t: dict[str, Any]) -> str:
    q = t["qualified"]
    return q if q.startswith("::") else "::" + q


def _is_opaque_arg(arg: dict[str, Any]) -> bool:
    return arg["type"].get("kind") == "opaque_class"


def _coroutine_param(arg: dict[str, Any]) -> str:
    """Pass opaque arguments by owning handle, never by value.

    Opaque instances are stored in the per-session object registry and may be
    deliberately non-copyable.  A coroutine frame must own the shared handle;
    passing a reference would also leave a dangling reference after dispatch
    returns.
    """
    if _is_opaque_arg(arg):
        return f"std::shared_ptr<void> {arg['name']}Obj"
    return f"{_cpp_type(arg['type'])} {arg['name']}"


def _coroutine_arg(arg: dict[str, Any]) -> str:
    if _is_opaque_arg(arg):
        return f"std::move({arg['name']}Obj)"
    if arg["type"].get("kind") == "string":
        return f"std::move({arg['name']})"
    return arg["name"]


def _opaque_aliases(args: list[dict[str, Any]], indent: str = "    ") -> str:
    lines = []
    for arg in args:
        if not _is_opaque_arg(arg):
            continue
        q = _opaque_type_qualified(arg["type"])
        lines.append(
            f"{indent}auto& {arg['name']} = *static_cast<{q}*>({arg['name']}Obj.get());"
        )
    return "\n".join(lines)


def _cpp_write_item(t: dict[str, Any], expr: str) -> str:
    """Return a C++ statement that writes `expr` of type `t` using ByteWriter `w`."""
    k = t.get("kind")
    if k == "i32":
        return f"w.i32({expr});"
    if k == "u32":
        return f"w.u32({expr});"
    if k == "u8":
        return f"w.u8({expr});"
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
    if k == "time_point":
        return (
            f"w.i64(static_cast<std::int64_t>("
            f"std::chrono::duration_cast<std::chrono::microseconds>("
            f"({expr}).time_since_epoch()).count()));"
        )
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
        if t["inner"].get("kind") == "u8":
            return f"w.u8vec({expr});"
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
    if k == "u8_ptr":
        # Raw byte pointer travels as its address (caller owns lifetime).
        return f"w.u64(reinterpret_cast<std::uint64_t>({expr}));"
    raise ValueError(f"unsupported C++ item type: {t}")


def _cpp_read_item(t: dict[str, Any], reader: str = "r") -> str:
    """Return a C++ expression that reads one value of type `t` from ByteReader `reader`."""
    k = t.get("kind")
    if k == "i32":
        return f"{reader}.i32()"
    if k == "u32":
        return f"{reader}.u32()"
    if k == "u8":
        return f"{reader}.u8()"
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
    if k == "time_point":
        return (
            f"std::chrono::system_clock::time_point{{"
            f"std::chrono::microseconds{{{reader}.i64()}}}}"
        )
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
        if t["inner"].get("kind") == "u8":
            return f"{reader}.u8vec()"
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
    if k == "u8_ptr":
        return f"reinterpret_cast<std::uint8_t*>({reader}.u64())"
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


def _type_has_u8_ptr(t: dict[str, Any]) -> bool:
    """Return True if `t` (recursively) references a uint8_t* (u8_ptr) type."""
    k = t.get("kind")
    if k == "u8_ptr":
        return True
    if k in ("vector", "array", "set", "optional", "lazy", "stream_sink"):
        return _type_has_u8_ptr(t.get("inner", {}))
    if k == "map":
        return _type_has_u8_ptr(t.get("key", {})) or _type_has_u8_ptr(t.get("value", {}))
    if k in ("pair", "tuple"):
        return any(_type_has_u8_ptr(e) for e in t.get("elements", []))
    if k == "dart_fn":
        return any(_type_has_u8_ptr(a) for a in t.get("args", [])) or _type_has_u8_ptr(t.get("return", {}))
    return False


def _fn_or_method_uses_u8_ptr(fn: dict[str, Any]) -> bool:
    """Return True if a function/method signature references u8_ptr."""
    if _type_has_u8_ptr(fn.get("return", {})):
        return True
    return any(_type_has_u8_ptr(a["type"]) for a in fn.get("args", []))


def _type_has_u8vec(t: dict[str, Any]) -> bool:
    """Return True if `t` (recursively) references a std::vector<uint8_t>
    (bulk-bytes) type — those surface as Uint8List and need dart:typed_data."""
    k = t.get("kind")
    if k == "vector":
        return t.get("inner", {}).get("kind") == "u8"
    if k in ("array", "set", "optional", "lazy", "stream_sink"):
        return _type_has_u8vec(t.get("inner", {}))
    if k == "map":
        return _type_has_u8vec(t.get("key", {})) or _type_has_u8vec(t.get("value", {}))
    if k in ("pair", "tuple"):
        return any(_type_has_u8vec(e) for e in t.get("elements", []))
    if k == "dart_fn":
        return any(_type_has_u8vec(a) for a in t.get("args", [])) or _type_has_u8vec(t.get("return", {}))
    return False


def _fn_or_method_uses_u8vec(fn: dict[str, Any]) -> bool:
    """Return True if a function/method signature references vector<uint8_t>."""
    if _type_has_u8vec(fn.get("return", {})):
        return True
    return any(_type_has_u8vec(a["type"]) for a in fn.get("args", []))


def _ir_uses_u8_ptr(ir: dict[str, Any]) -> bool:
    """Return True if any exported function/field references a u8_ptr type."""
    for fn in ir.get("functions", []):
        if _fn_or_method_uses_u8_ptr(fn):
            return True
    for cls in ir.get("classes", []):
        for m in cls.get("methods", []):
            if _fn_or_method_uses_u8_ptr(m):
                return True
        for f in cls.get("fields", []):
            if _type_has_u8_ptr(f["type"]):
                return True
    return False


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
    instance, static). Returns (cases, sync_cases)."""
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
                a for a in m["args"]
                if a["type"].get("kind") != "stream_sink"
                and not _is_optional_sink_arg(a)
            ]
            # Optional sink args surface as `std::move(sink)` at their original
            # parameter position (the sink is constructed in the dispatch body).
            arg_names = [
                "std::move(sink)" if _is_optional_sink_arg(a) else a["name"]
                for a in m["args"]
                if a["type"].get("kind") != "stream_sink"
            ]
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
        const auto handle = dcb::ObjectHandleRegistry::instance().insert_for_session(session_id, obj, [session_id](std::shared_ptr<void>&) {{
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
    try {{
      auto obj = {ctor_call};
      {counter_var}.increment(session_id);
      const auto handle = dcb::ObjectHandleRegistry::instance().insert_for_session(session_id, obj, [session_id](std::shared_ptr<void>&) {{
        {counter_var}.decrement(session_id);
      }});
      ByteWriter w;
      w.u64(handle);
      return make_frame(MsgType::kResponseOk, frame.request_id, frame.method_id, w.raw());
    }} catch (const std::exception& e) {{
      ByteWriter ew;
      ew.i32(1);
      ew.str(dcb::error::format("{class_name}::{class_name}", e.what()));
      return make_frame(MsgType::kResponseErr, frame.request_id, frame.method_id, ew.raw());
    }} catch (...) {{
      ByteWriter ew;
      ew.i32(1);
      ew.str(dcb::error::format("{class_name}::{class_name}", "unknown"));
      return make_frame(MsgType::kResponseErr, frame.request_id, frame.method_id, ew.raw());
    }}
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
        auto obj = dcb::ObjectHandleRegistry::instance().get(session_id, handle);
        if (!obj) {{
          post_err(session, gen, req, method, "{fn_label}", "{err_msg}");
          break;
        }}
        {arg_reads}"""
                sync_handle_block = f"""const auto handle = r.u64();
        auto obj = dcb::ObjectHandleRegistry::instance().get(session_id, handle);
        if (!obj) {{
          ByteWriter ew;
          ew.i32(1);
          ew.str(dcb::error::format("{fn_label}", "{err_msg}"));
          return make_frame(MsgType::kResponseErr, frame.request_id, frame.method_id, ew.raw());
        }}
        {sync_arg_reads}"""
                call = f"static_cast<{class_q}*>(obj.get())->{m['name']}({', '.join(arg_names)})"

            write = _cpp_write_ret(ret, "out") if kind != "stream" else ""
            # void 返回值不能赋给变量
            sync_call_stmt = f"{call};" if ret.get("kind") == "void" else f"auto out = {call};"

            if kind == "sync":
                body = f"""
      case {mid}: {{
        ByteReader r(frame.payload.data(), frame.payload.size());
        {handle_block}
        ByteWriter w;
        {{
          {sync_call_stmt}
          {write}
        }}
        post_ok(session, gen, req, method, w.raw());
        break;
      }}"""
                sync_body = f"""
  if (frame.method_id == {mid}u) {{
    ByteReader r(frame.payload.data(), frame.payload.size());
    {sync_handle_block}
    try {{
      ByteWriter w;
      {{
        {sync_call_stmt}
        {write}
      }}
      return make_frame(MsgType::kResponseOk, frame.request_id, frame.method_id, w.raw());
    }} catch (const std::exception& e) {{
      ByteWriter ew;
      ew.i32(1);
      ew.str(dcb::error::format("{fn_label}", e.what()));
      return make_frame(MsgType::kResponseErr, frame.request_id, frame.method_id, ew.raw());
    }} catch (...) {{
      ByteWriter ew;
      ew.i32(1);
      ew.str(dcb::error::format("{fn_label}", "unknown"));
      return make_frame(MsgType::kResponseErr, frame.request_id, frame.method_id, ew.raw());
    }}
  }}"""
                cases.append(body)
                sync_cases.append(sync_body)

            elif kind == "async":
                # Lazy-coroutine lambda with IMMEDIATE invocation (IIFE):
                # plain capture in a lazy coroutine is a dangling read once the
                # closure dies before the coroutine body first runs — a
                # language-level lifetime rule, not compiler-specific (see
                # docs/known_issues.md ID-022). All state is passed in by
                # value as coroutine parameters (copied into the coroutine
                # frame), so the lambda itself must not capture anything.
                fn_params = ["std::shared_ptr<Session> session", "std::uint64_t session_id",
                             "std::uint64_t gen", "std::uint64_t req", "std::uint32_t method"]
                fn_args = ["session", "session_id", "gen", "req", "method"]
                if not is_static:
                    fn_params.append("std::shared_ptr<void> obj")
                    fn_args.append("obj")
                for a in non_sink_args:
                    fn_params.append(_coroutine_param(a))
                    fn_args.append(_coroutine_arg(a))
                # Optional sink setup (read stream_id, create sink if non-zero).
                # Mirrors the free-function async branch.
                sink_setup = ""
                opt_sink_arg = next(
                    (a for a in m["args"] if _is_optional_sink_arg(a)), None
                )
                if opt_sink_arg:
                    sink_inner = opt_sink_arg["type"]["inner"]["inner"]
                    sink_encode = _cpp_write_item(sink_inner, "v")
                    sink_setup = f"""
        const auto _stream_id = r.u64();
        std::optional<dcb::StreamSink<{_cpp_type(sink_inner)}>> sink;
        if (_stream_id != 0) {{
          sink.emplace(session, _stream_id, gen, method, []({_cpp_type(sink_inner)} v) {{
            ByteWriter w;
            {sink_encode}
            return w.raw();
          }});
        }}"""
                    fn_params.append(
                        f"std::optional<dcb::StreamSink<{_cpp_type(sink_inner)}>> sink"
                    )
                    fn_args.append("std::move(sink)")
                call_stmt = f"co_await {call};" if ret.get("kind") == "void" else f"auto out = co_await {call};"
                fn_label = f"{class_name}::{m['name']}"
                aliases = _opaque_aliases(non_sink_args)
                iife = f"""[]( {', '.join(fn_params)}) -> stdexec::task<void> {{
{aliases}
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
}}({', '.join(fn_args)})"""
                body = f"""
      case {mid}: {{
        ByteReader r(frame.payload.data(), frame.payload.size());
        {handle_block}{sink_setup}
        auto task = {iife};
        spawn_on_io(std::move(task));
        break;
      }}"""
                cases.append(body)

            elif kind == "normal":
                move_caps = ", ".join(
                    (
                        f"{a['name']}Obj = std::move({a['name']}Obj)"
                        if _is_opaque_arg(a)
                        else (
                            f"{a['name']} = std::move({a['name']})"
                            if a["type"].get("kind") == "string"
                            else a["name"]
                        )
                    )
                    for a in non_sink_args
                )
                obj_cap = "" if is_static else "handle, obj"
                lambda_caps = ", ".join(
                    x for x in (obj_cap, move_caps) if x
                )
                ret_kind = ret.get("kind")
                if ret_kind == "void":
                    ret_cpp = "dcb::Unit"
                    call_stmt = f"{call};"
                    encode_lambda = "[](ByteWriter& w, dcb::Unit&&) { (void)w; }"
                else:
                    ret_cpp = _cpp_type(ret)
                    call_stmt = f"return {call};"
                    encode_capture = "[session_id]" if ret_kind == "opaque_class" else "[]"
                    encode_lambda = f"""{encode_capture}(ByteWriter& w, auto&& out) {{
              {write}
            }}"""
                fn_label = f"{class_name}::{m['name']}"
                aliases = _opaque_aliases(non_sink_args, indent="              ")
                body = f"""
      case {mid}: {{
        ByteReader r(frame.payload.data(), frame.payload.size());
        {handle_block}
        run_async<{ret_cpp}>(
            session, gen, req, method,
            dcb::spawn_blocking([{lambda_caps}]() {{
              {aliases}
              {call_stmt}
            }}),
            {encode_lambda},
            "{fn_label}");
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
        auto sink = dcb::StreamSink<{_cpp_type(sink_inner)}>(session, req, gen, method, []({_cpp_type(sink_inner)} v) {{
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


def _dart_data_class_defs(
    classes: list[dict[str, Any]],
    custom_code: dict[str, str] | None = None,
) -> str:
    """Generate immutable Dart data classes with const constructors.

    Each class gets a default `toString()`. If `custom_code` provides an entry
    for a class name, that Dart code is injected verbatim at the end of the
    class body instead (and the default toString is suppressed).
    """
    custom_code = custom_code or {}
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
            is_nullable = f["type"].get("kind") == "optional"
            if is_nullable:
                ctor_lines.append(f"    this.{dart_name},")
            else:
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

        # Class-body tail: custom dart_code (verbatim) or a default toString.
        user_code = custom_code.get(name)
        if user_code:
            code_lines = [
                f"  {ln}" if ln.strip() else ""
                for ln in str(user_code).rstrip("\n").split("\n")
            ]
            tail = "\n" + "\n".join(code_lines) + "\n"
        else:
            ts_parts = ", ".join(f"{dn}: ${dn}" for dn in hash_fields)
            tail = (
                "\n  @override\n"
                f"  String toString() => '{name}({ts_parts})';\n"
            )

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
{tail}}}"""
        )
    return "\n\n".join(defs)


def _dart_data_class_helpers(classes: list[dict[str, Any]]) -> str:
    """Generate Dart encode/decode helpers for every data_class."""
    lines: list[str] = []
    for cls in classes:
        name = cls["name"]
        fields = cls["fields"]
        lines.append(f"void _writeDataClass{name}(ByteWriter w, {name} v) {{")
        for f in fields:
            dart_name = _dart_param_name(f["name"])
            lines.extend(
                _dart_write_item(f["type"], f"v.{dart_name}", indent="  ", writer="w")
            )
        lines.append("}")
        lines.append("")
        lines.append(f"{name} _readDataClass{name}(ByteReader _r) {{")
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
    if k in ("i32", "u32", "i64", "u8", "bool", "string", "enum", "f32", "f64", "data_class", "time_point", "u8_ptr"):
        return f"const auto {name} = {_cpp_read_item(t)};"
    if k == "optional":
        inner = t["inner"]
        inner_t = _cpp_type(inner)
        return f"const auto {name} = r.opt<{inner_t}>([&]() {{ return {_cpp_read_item(inner)}; }});"
    if k == "vector":
        if t["inner"].get("kind") == "u8":
            return f"const auto {name} = r.u8vec();"
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
        if t.get("ordered"):
            return f"const auto {name} = r.set<{inner_t}, std::set>([&]() {{ return {_cpp_read_item(inner)}; }});"
        return f"const auto {name} = r.set<{inner_t}>([&]() {{ return {_cpp_read_item(inner)}; }});"
    if k == "map":
        key_t = _cpp_type(t["key"])
        value_t = _cpp_type(t["value"])
        if t.get("ordered"):
            return (
                f"const auto {name} = r.map<{key_t}, {value_t}, std::map>("
                f"[&]() {{ return {_cpp_read_item(t['key'])}; }}, "
                f"[&]() {{ return {_cpp_read_item(t['value'])}; }});"
            )
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
                f"    auto {name}Obj = dcb::ObjectHandleRegistry::instance().get(session_id, {name}Handle);\n"
                f"    if (!{name}Obj) {{\n"
                f"      ByteWriter ew; ew.i32(1); ew.str(\"{err_msg}\");\n"
                f"      return make_frame(MsgType::kResponseErr, frame.request_id, frame.method_id, ew.raw());\n"
                f"    }}\n"
                f"    {q}& {name} = *static_cast<{q}*>({name}Obj.get());"
            )
        return (
            f"const auto {name}Handle = r.u64();\n"
            f"        auto {name}Obj = dcb::ObjectHandleRegistry::instance().get(session_id, {name}Handle);\n"
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
    if k in ("i32", "u32", "i64", "u8", "bool", "string", "enum", "f32", "f64", "data_class", "time_point", "u8_ptr"):
        return _cpp_write_item(t, expr)
    if k == "optional":
        inner = t["inner"]
        item = _cpp_write_item(inner, "v")
        return f"w.opt({expr}, [&](const auto& v) {{ {item} }});"
    if k == "vector":
        if t["inner"].get("kind") == "u8":
            return f"w.u8vec({expr});"
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
            f"const auto __handle = dcb::ObjectHandleRegistry::instance().insert_for_session("
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
    if k == "u8":
        return [f"{indent}{writer}.u8({expr});"]
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
    if k == "time_point":
        return [f"{indent}{writer}.i64({expr}.microsecondsSinceEpoch);"]
    if k == "enum":
        return [f"{indent}{writer}.i32({expr}.index);"]
    if k == "data_class":
        return [f"{indent}_writeDataClass{t['name']}({writer}, {expr});"]
    if k == "opaque_class":
        return [f"{indent}{writer}.u64({expr});"]
    if k == "i128":
        return [f"{indent}{writer}.writeI128({expr});"]
    if k == "u128":
        return [f"{indent}{writer}.writeU128({expr});"]
    if k == "u8_ptr":
        return [f"{indent}{writer}.u64({expr}.address);"]
    if k == "optional":
        inner = t["inner"]
        # Dart flow analysis promotes simple variables in the else-branch,
        # but not property accesses (e.g. v.field) — keep `!` only for those.
        promoted = expr if expr.isidentifier() else f"{expr}!"
        return [
            f"{indent}if ({expr} == null) {{ {writer}.u8(0); }} else {{ {writer}.u8(1);",
            *_dart_write_item(inner, promoted, indent + "  ", writer),
            f"{indent}}}",
        ]
    if k == "vector":
        if t["inner"].get("kind") == "u8":
            return [f"{indent}{writer}.u8vec({expr});"]
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
    if k == "u8":
        return f"{reader}.u8()"
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
    if k == "time_point":
        return f"DateTime.fromMicrosecondsSinceEpoch({reader}.i64(), isUtc: true)"
    if k == "enum":
        return f"{t['name']}.values[{reader}.i32()]"
    if k == "data_class":
        return f"_readDataClass{t['name']}({reader})"
    if k == "opaque_class":
        return f"{reader}.u64()"
    if k == "u128":
        return f"{reader}.readU128()"
    if k == "u8_ptr":
        return f"Pointer<Uint8>.fromAddress({reader}.u64())"
    if k == "optional":
        inner = t["inner"]
        read_value = _dart_read_item(inner, reader)
        return f"(({reader}.u8() != 0) ? {read_value} : null)"
    if k == "vector":
        if t["inner"].get("kind") == "u8":
            return f"{reader}.u8vec()"
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
    if k == "u8":
        return f"ByteReader({expr}).u8()"
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
    if k == "time_point":
        return f"DateTime.fromMicrosecondsSinceEpoch(ByteReader({expr}).i64(), isUtc: true)"
    if k == "enum":
        return f"{t['name']}.values[ByteReader({expr}).i32()]"
    if k == "data_class":
        return f"_readDataClass{t['name']}(ByteReader({expr}))"
    if k == "opaque_class":
        return f"ByteReader({expr}).u64()"
    if k == "optional":
        inner = t["inner"]
        read_value = _dart_read_item(inner, "_r")
        return f"(() {{ final _r = ByteReader({expr}); final _has = _r.u8() != 0; return _has ? {read_value} : null; }})()"
    if k == "vector":
        if t["inner"].get("kind") == "u8":
            return f"(() {{ final _r = ByteReader({expr}); return _r.u8vec(); }})()"
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
    if k == "u8_ptr":
        return f"Pointer<Uint8>.fromAddress(ByteReader({expr}).u64())"
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
        elif k == "u8_ptr":
            lines.append(f"_payload.u64({n}.address);")
        elif k in ("i32", "u32", "i64", "u8", "string", "bool", "enum", "f32", "f64", "data_class", "time_point"):
            lines.extend(_dart_write_item(t, n))
        elif k == "optional":
            inner = t["inner"]
            lines.append(f"if ({n} == null) {{ _payload.u8(0); }} else {{ _payload.u8(1);")
            lines.extend(_dart_write_item(inner, n, "  "))
            lines.append("}")
        elif k == "vector":
            if t["inner"].get("kind") == "u8":
                lines.append(f"_payload.u8vec({n});")
                continue
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
                p = f"Future<{ret_t}> Function({arg_types}) {dn}"
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
        # void 返回值不能赋给变量
        sync_call_stmt = f"{call};" if fn["return"].get("kind") == "void" else f"auto out = {call};"

        if kind == "sync":
            has_dart_fn_arg = any(
                a["type"].get("kind") == "dart_fn" for a in fn["args"]
            )
            # Optional sink setup (read stream_id, create sink if non-zero).
            # Mirrors the async/normal branches; the sync dispatch path needs
            # the session too, so it is looked up from the registry.
            sink_setup = ""
            sync_sink_setup = ""
            if opt_sink_arg:
                sink_inner = opt_sink_arg["type"]["inner"]["inner"]
                sink_encode = _cpp_write_item(sink_inner, "v")
                sink_setup = f"""
        const auto _stream_id = r.u64();
        std::optional<dcb::StreamSink<{_cpp_type(sink_inner)}>> sink;
        if (_stream_id != 0) {{
          sink.emplace(session, _stream_id, gen, method, []({_cpp_type(sink_inner)} v) {{
            ByteWriter w;
            {sink_encode}
            return w.raw();
          }});
        }}"""
                sync_sink_setup = f"""
    const auto _stream_id = r.u64();
    std::optional<dcb::StreamSink<{_cpp_type(sink_inner)}>> sink;
    if (_stream_id != 0) {{
      sink.emplace(session, _stream_id, gen, frame.method_id, []({_cpp_type(sink_inner)} v) {{
        ByteWriter w;
        {sink_encode}
        return w.raw();
      }});
    }}"""
            body = f"""
      case {mid}: {{
        ByteReader r(frame.payload.data(), frame.payload.size());
        {reads}{sink_setup}
        ByteWriter w;
        {{
          {sync_call_stmt}
          {write}
        }}
        post_ok(session, gen, req, method, w.raw());
        break;
      }}"""
            cases.append(body)
            session_lookup = ""
            if has_dart_fn_arg or opt_sink_arg:
                session_lookup = (
                    "\n    auto session = dcb::SessionRegistry::instance().get(session_id);"
                    "\n    auto gen = session->generation();"
                )
            sync_body = f"""
  if (frame.method_id == {mid}u) {{{session_lookup}
    ByteReader r(frame.payload.data(), frame.payload.size());
    {sync_reads}{sync_sink_setup}
    try {{
      ByteWriter w;
      {{
        {sync_call_stmt}
        {write}
      }}
      return make_frame(MsgType::kResponseOk, frame.request_id, frame.method_id, w.raw());
    }} catch (const std::exception& e) {{
      ByteWriter ew;
      ew.i32(1);
      ew.str(dcb::error::format("{fn['name']}", e.what()));
      return make_frame(MsgType::kResponseErr, frame.request_id, frame.method_id, ew.raw());
    }} catch (...) {{
      ByteWriter ew;
      ew.i32(1);
      ew.str(dcb::error::format("{fn['name']}", "unknown"));
      return make_frame(MsgType::kResponseErr, frame.request_id, frame.method_id, ew.raw());
    }}
  }}"""
            sync_cases.append(sync_body)

        elif kind == "async":
            # Lazy-coroutine lambda with IMMEDIATE invocation (IIFE): plain
            # capture in a lazy coroutine is a dangling read once the closure
            # dies before the coroutine body first runs — a language-level
            # lifetime rule, not compiler-specific (docs/known_issues.md
            # ID-022). All state is passed in by value as coroutine parameters
            # (copied into the coroutine frame); the lambda must not capture.
            fn_params = ["std::shared_ptr<Session> session", "std::uint64_t gen",
                         "std::uint64_t req", "std::uint32_t method"]
            fn_args = ["session", "gen", "req", "method"]
            for a in fn["args"]:
                if a["type"].get("kind") == "stream_sink" or _is_optional_sink_arg(a):
                    continue
                fn_params.append(_coroutine_param(a))
                fn_args.append(_coroutine_arg(a))

            # Optional sink setup (read stream_id, create sink if non-zero).
            sink_setup = ""
            if opt_sink_arg:
                sink_inner = opt_sink_arg["type"]["inner"]["inner"]
                sink_encode = _cpp_write_item(sink_inner, "v")
                sink_setup = f"""
        const auto _stream_id = r.u64();
        std::optional<dcb::StreamSink<{_cpp_type(sink_inner)}>> sink;
        if (_stream_id != 0) {{
          sink.emplace(session, _stream_id, gen, method, []({_cpp_type(sink_inner)} v) {{
            ByteWriter w;
            {sink_encode}
            return w.raw();
          }});
        }}"""
                fn_params.append(f"std::optional<dcb::StreamSink<{_cpp_type(sink_inner)}>> sink")
                fn_args.append("std::move(sink)")

            ret_kind = fn["return"].get("kind")
            if ret_kind == "void":
                call_stmt = f"co_await {call};"
            else:
                call_stmt = f"auto out = co_await {call};"

            aliases = _opaque_aliases(
                [
                    a
                    for a in fn["args"]
                    if a["type"].get("kind") != "stream_sink"
                    and not _is_optional_sink_arg(a)
                ]
            )
            iife = f"""[]( {', '.join(fn_params)}) -> stdexec::task<void> {{
{aliases}
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
}}({', '.join(fn_args)})"""

            body = f"""
      case {mid}: {{
        ByteReader r(frame.payload.data(), frame.payload.size());
        {reads}{sink_setup}
        auto task = {iife};
        spawn_on_io(std::move(task));
        break;
      }}"""
            cases.append(body)

        elif kind == "normal":
            move_caps = ", ".join(
                (
                    f"{a['name']}Obj = std::move({a['name']}Obj)"
                    if _is_opaque_arg(a)
                    else (
                        f"{a['name']} = std::move({a['name']})"
                        if a["type"].get("kind") == "string"
                        else a["name"]
                    )
                )
                for a in fn["args"]
                if a["type"].get("kind") != "stream_sink" and not _is_optional_sink_arg(a)
            )
            lambda_caps = move_caps

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
          sink.emplace(session, _stream_id, gen, method, []({_cpp_type(sink_inner)} v) {{
            ByteWriter w;
            {sink_encode}
            return w.raw();
          }});
        }}"""
                sink_capture = ", sink = std::move(sink)"
                lambda_caps = ", ".join(
                    x for x in (lambda_caps, "sink = std::move(sink)") if x
                )

            ret_kind = fn["return"].get("kind")
            if ret_kind == "void":
                ret_cpp = "dcb::Unit"
                call_stmt = f"{call};"
                encode_lambda = "[](ByteWriter& w, dcb::Unit&&) { (void)w; }"
            else:
                ret_cpp = _cpp_type(fn["return"])
                call_stmt = f"return {call};"
                encode_capture = "[session_id]" if ret_kind == "opaque_class" else "[]"
                encode_lambda = f"""{encode_capture}(ByteWriter& w, auto&& out) {{
              {write}
            }}"""

            aliases = _opaque_aliases(
                [
                    a
                    for a in fn["args"]
                    if a["type"].get("kind") != "stream_sink"
                    and not _is_optional_sink_arg(a)
                ],
                indent="              ",
            )

            body = f"""
      case {mid}: {{
        ByteReader r(frame.payload.data(), frame.payload.size());
        {reads}{sink_setup}
        run_async<{ret_cpp}>(
            session, gen, req, method,
            dcb::spawn_blocking([{lambda_caps}]() {{
              {aliases}
              {call_stmt}
            }}),
            {encode_lambda},
            "{fn['name']}");
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
        auto sink = dcb::StreamSink<{_cpp_type(sink_inner)}>(session, req, gen, method, []({_cpp_type(sink_inner)} v) {{
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

    # Diagnostic aid (not for production): DCB_GEN_MAX_DISPATCH truncates the
    # generated dispatch cases so MSVC template-bloat OOM can be bisected by
    # compiling increasingly large slices.
    import os as _os

    _max_dispatch = _os.environ.get("DCB_GEN_MAX_DISPATCH")
    if _max_dispatch:
        _n = int(_max_dispatch)
        cases = cases[:_n]
        cases_s = "\n".join(cases) if cases else ""

    cpp = f"""// GENERATED by dart_cpp_bridge codegen — do not edit.
#include "wire_dispatch.hpp"

#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/dispatch.hpp"
#include "dart_cpp_bridge/error_config.hpp"
#include "dart_cpp_bridge/object_handle.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"
#include "dart_cpp_bridge/start_with_receiver.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

{inc_lines}

#include <stdexec/execution.hpp>
#include <exec/start_detached.hpp>

#include <chrono>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dcb {{
namespace demo {{

namespace {{

// The environment starts_on(io_scheduler, sndr) actually provides to the
// child sender: get_scheduler and get_start_scheduler both answer the io
// scheduler. stdexec::task's completion signatures are env-independent, but
// `sender_in<S, spawn_env_t>` is kept so non-senders are rejected with a
// clear compile error at the call site while stdexec::task is accepted.
using spawn_env_t = decltype(stdexec::env{{
    stdexec::prop{{stdexec::get_scheduler,
                   std::declval<const dcb::IoContextScheduler&>()}},
    stdexec::prop{{stdexec::get_start_scheduler,
                   std::declval<const dcb::IoContextScheduler&>()}}}});

// Launch a dispatch coroutine on the bridge io thread. The official
// exec::start_detached terminates on set_error, so an upon_error log is
// appended (the coroutine bodies below catch everything anyway). Every
// dispatch function returns stdexec::task<void>, so S deduces to the same type
// at every call site and the chain below instantiates exactly once.
template <class S>
  requires stdexec::sender_in<S, spawn_env_t>
void spawn_on_io(S&& sndr) {{
  exec::start_detached(
      stdexec::starts_on(*dcb::Runtime::instance().io_scheduler(),
                         std::forward<S>(sndr)) |
      stdexec::upon_error([](std::exception_ptr ep) noexcept {{
          try {{
            std::rethrow_exception(ep);
          }} catch (const std::exception& e) {{
            std::fprintf(stderr, "[wire] detached sender error: %s\\n", e.what());
          }} catch (...) {{
            std::fprintf(stderr, "[wire] detached sender error: unknown\\n");
          }}
        }}) |
      stdexec::upon_stopped([]() noexcept {{
        std::fprintf(stderr, "[wire] detached sender stopped\\n");
      }}));
}}

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

// Receiver that turns a sender's completion into a Dart response frame:
// set_value -> responseOk, set_error / set_stopped -> responseErr.
// Same pattern as examples/base_demo/demo_api.cpp.
template <typename T>
struct DispatchReceiver {{
  using receiver_concept = stdexec::receiver_tag;

  std::shared_ptr<Session> session;
  std::uint64_t gen{{0}};
  std::uint64_t req{{0}};
  std::uint32_t method{{0}};
  std::string name;
  std::function<void(ByteWriter&, T&&)> encode;

  void set_value(T v) && noexcept {{
    try {{
      ByteWriter w;
      encode(w, std::move(v));
      post_ok(session, gen, req, method, w.raw());
    }} catch (const std::exception& e) {{
      post_err(session, gen, req, method, name.c_str(), e.what());
    }} catch (...) {{
      post_err(session, gen, req, method, name.c_str(), "unknown");
    }}
  }}

  void set_error(std::exception_ptr ep) && noexcept {{
    std::string msg = "unknown";
    try {{
      std::rethrow_exception(ep);
    }} catch (const std::exception& e) {{
      msg = e.what();
    }} catch (...) {{
    }}
    post_err(session, gen, req, method, name.c_str(), msg);
  }}

  void set_stopped() && noexcept {{
    post_err(session, gen, req, method, name.c_str(), "sender stopped");
  }}
}};

// Offload a blocking business call to the thread pool via dcb::spawn_blocking
// (its completion is delivered back on the io thread) and route the result
// into a response frame. Replaces the old asio::post(pool) + asio::post(io)
// double hop; exceptions from the business call become responseErr frames.
template <typename T, stdexec::sender S, typename Encode>
void run_async(const std::shared_ptr<Session>& session, std::uint64_t gen,
               std::uint64_t req, std::uint32_t method, S&& sndr,
               Encode&& encode, const char* name) {{
  try {{
    auto chain = stdexec::starts_on(*dcb::Runtime::instance().io_scheduler(),
                                    std::forward<S>(sndr));
    auto rcvr = DispatchReceiver<T>{{
        session, gen, req, method, name,
        std::function<void(ByteWriter&, T&&)>(std::forward<Encode>(encode))}};
    dcb::start_with_receiver(std::move(chain), std::move(rcvr));
  }} catch (const std::exception& e) {{
    post_err(session, gen, req, method, name, e.what());
  }} catch (...) {{
    post_err(session, gen, req, method, name, "unknown");
  }}
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
    return _dart_identifier(cpp_name)


def _dart_fn_name(cpp_name: str) -> str:
    return _dart_identifier(cpp_name)


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


def _dart_fn_wrapper_lines(
    a: dict[str, Any], *, persistent_key: str | None = None
) -> list[str]:
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
    if ret.get("kind") == "void":
        lines.append(f"  await {name}({', '.join(arg_names)});")
        lines.append("  return Uint8List(0);")
    else:
        lines.append(f"  final _res = await {name}({', '.join(arg_names)});")
        lines.append("  final _w = ByteWriter();")
        for stmt in _dart_write_item(ret, "_res", indent="  ", writer="_w"):
            lines.append(stmt)
        lines.append("  return _w.takeBytes();")
    lines.append("};")
    if persistent_key is None:
        lines.append(f"final _{name}Id = bridge.registerDartFn({wrapper_name});")
    else:
        lines.append(
            f"final _{name}Id = bridge.registerPersistentDartFn("
            f"'{persistent_key}', {wrapper_name});"
        )
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
        is_persist = dart_fn_try and "bridge::persist" in m["fn"].get("attrs", [])
        if dart_fn_try:
            if m["is_stream"]:
                raise ValueError(
                    f"DartFn callbacks inside stream methods are not supported: {m['fn']['qualified']}"
                )
            for a in dart_fn_args:
                persistent_key = (
                    f"{m['fn']['qualified']}::{a['dart_name']}"
                    if is_persist
                    else None
                )
                body_lines.extend(
                    _dart_fn_wrapper_lines(a, persistent_key=persistent_key)
                )
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
            # Optional StreamSink (progress events + result). Async methods
            # await the response; sync methods block in the FFI call and the
            # stream events queued on the reply port are delivered afterwards.
            osi = m["opt_sink_info"]
            for line in payload_lines:
                body_lines.append(f"{indent}{line}")
            decode_item = osi["decode_expr"]
            dart_item_t = osi["dart_item_type"]
            ctrl_name = osi["dart_name"]
            if not m["is_async"]:
                # Sync method with optional stream.
                if ret_t == "void":
                    body_lines.append(
                        f"{indent}bridge.invokeSyncMethodWithStream<{dart_item_t}>("
                        f"{dart_name}Id, _payload, {ctrl_name}, "
                        f"(final _r) => {decode_item});"
                    )
                else:
                    body_lines.append(
                        f"{indent}final _bytes = bridge.invokeSyncMethodWithStream<{dart_item_t}>("
                        f"{dart_name}Id, _payload, {ctrl_name}, "
                        f"(final _r) => {decode_item});"
                    )
                    body_lines.append(f"{indent}return {read_ret};")
            else:
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
            if is_persist:
                body_lines.append("} finally {")
                body_lines.append("  // BRIDGE_PERSIST: retained until the next registration for this key.")
                body_lines.append("}")
            else:
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
            opt_sink_info_cls = None  # optional StreamSink arg (progress events)
            if is_instance:
                params.append(f"{cls['name']} self")
                call_args.append("self")
                ensure_alive_lines_cls.append("self.ensureAlive();")
            for a in method["args"]:
                t = a["type"]
                if t.get("kind") == "stream_sink":
                    continue
                dart_name = a["dart_name"]
                if _is_optional_sink_arg(a):
                    # Optional sink → StreamController<T>? parameter; the sink
                    # itself is constructed natively from the stream id.
                    item_t = t["inner"]["inner"]
                    params.append(f"StreamController<{_dart_type(item_t)}>? {dart_name}")
                    opt_sink_info_cls = {
                        "dart_name": dart_name,
                        "dart_item_type": _dart_type(item_t),
                        "decode_expr": _dart_read_item(item_t, "_r"),
                    }
                    continue
                if t.get("kind") == "opaque_class":
                    params.append(f"{t['name']} {dart_name}")
                    ensure_alive_lines_cls.append(f"{dart_name}.ensureAlive();")
                elif t.get("kind") == "dart_fn":
                    arg_types = ", ".join(_dart_type(arg_t) for arg_t in t.get("args", []))
                    ret_t = _dart_type(t["return"])
                    params.append(f"Future<{ret_t}> Function({arg_types}) {dart_name}")
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
                if _is_optional_sink_arg(a):
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
            is_persist = dart_fn_try and "bridge::persist" in method.get("attrs", [])
            if dart_fn_try:
                if is_stream:
                    raise ValueError(
                        f"DartFn callbacks inside stream methods are not supported: {cls['qualified']}::{method['name']}"
                    )
                for a in dart_fn_args:
                    persistent_key = (
                        f"{method['qualified']}::{a['dart_name']}"
                        if is_persist
                        else None
                    )
                    body_lines.extend(
                        _dart_fn_wrapper_lines(a, persistent_key=persistent_key)
                    )
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
            elif opt_sink_info_cls:
                # Optional StreamSink on an async method (progress events +
                # Future result), mirrors the free-function path.
                if not is_async:
                    raise ValueError(
                        "optional StreamSink on sync class methods is not supported: "
                        f"{cls['qualified']}::{method['name']}"
                    )
                for line in payload_lines:
                    body_lines.append(f"{indent}{line}")
                osi = opt_sink_info_cls
                if ret_t == "void":
                    body_lines.append(
                        f"{indent}await bridge.invokeAsyncMethodWithStream<{osi['dart_item_type']}>("
                        f"{impl_name}Id, _payload, {osi['dart_name']}, "
                        f"(final _r) => {osi['decode_expr']});"
                    )
                else:
                    body_lines.append(
                        f"{indent}final _bytes = await bridge.invokeAsyncMethodWithStream<{osi['dart_item_type']}>("
                        f"{impl_name}Id, _payload, {osi['dart_name']}, "
                        f"(final _r) => {osi['decode_expr']});"
                    )
                    body_lines.append(f"{indent}return {read_ret};")
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
                if is_persist:
                    body_lines.append("} finally {")
                    body_lines.append("  // BRIDGE_PERSIST: retained until the next registration for this key.")
                    body_lines.append("}")
                else:
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
    data_class_helpers = _dart_data_class_helpers(data_classes)
    if data_class_helpers:
        data_class_helpers += "\n"

    # Data class definitions and opaque wrappers live in per-header API files.
    # Collect imports for any API file whose types are referenced here (codec
    # helpers reference data classes; BridgeApiImpl references opaque classes).
    from pathlib import PurePosixPath, PureWindowsPath
    api_imports: list[str] = []
    seen_headers: set[str] = set()
    for cls in opaque_classes + data_classes:
        h = cls.get("header", "")
        if h and h not in seen_headers:
            seen_headers.add(h)
            hp = PureWindowsPath(h) if "\\" in h else PurePosixPath(h)
            dart_fname = hp.stem + ".dart"
            api_imports.append(f"import '{api_subdir}/{dart_fname}';")
    api_imports_s = "\n".join(sorted(api_imports))
    if api_imports_s:
        api_imports_s += "\n"

    # uint8_t* params/returns surface as Pointer<Uint8>, which needs dart:ffi.
    u8_ptr_import = "import 'dart:ffi';\n" if _ir_uses_u8_ptr(ir) else ""

    return f"""// GENERATED by dart_cpp_bridge codegen — do not edit.
// ignore_for_file: unused_element, unused_import

{u8_ptr_import}import 'dart:async';
import 'dart:typed_data';

import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';

{api_imports_s}{enums_s}
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


def generate_dart_bindings(package_name: str) -> str:
    """Generate dcb_bindings.dart — @Native externals for the dcb_* C ABI.

    The assetId is derived from [package_name] so that it matches the
    hook-registered code asset (hook/build.dart assetName).
    """
    asset_id = f"package:{package_name}/src/native_gen/dcb_bindings.dart"
    return f'''// GENERATED by dart_cpp_bridge codegen — do not edit.
// ignore_for_file: unused_element

import 'dart:ffi';

import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';
import 'package:ffi/ffi.dart';

const _kAssetId = '{asset_id}';

@Native<InitDartApiC>(assetId: _kAssetId, symbol: 'dcb_init_dart_api')
external int _dcbInitDartApi(Pointer<Void> data);

@Native<SessionOpenC>(assetId: _kAssetId, symbol: 'dcb_session_open')
external int _dcbSessionOpen(int replyNativePort);

@Native<SessionCloseC>(assetId: _kAssetId, symbol: 'dcb_session_close')
external void _dcbSessionClose(int sessionId);

@Native<ShutdownC>(assetId: _kAssetId, symbol: 'dcb_shutdown')
external void _dcbShutdown();

@Native<InvokeSyncC>(assetId: _kAssetId, symbol: 'dcb_invoke_sync')
external Pointer<Uint8> _dcbInvokeSync(
  int sessionId,
  Pointer<Uint8> req,
  int reqLen,
  Pointer<IntPtr> outLen,
  Pointer<Pointer<Utf8>> errorOut,
);

@Native<InvokeAsyncC>(assetId: _kAssetId, symbol: 'dcb_invoke_async')
external void _dcbInvokeAsync(int sessionId, Pointer<Uint8> req, int reqLen);

@Native<StreamCloseC>(assetId: _kAssetId, symbol: 'dcb_stream_close')
external void _dcbStreamClose(int sessionId, int streamId);

@Native<DartFnReplyC>(assetId: _kAssetId, symbol: 'dcb_dart_fn_reply')
external void _dcbDartFnReply(
  int sessionId,
  int replyId,
  int ok,
  Pointer<Uint8> payload,
  int payloadLen,
  Pointer<Utf8> errorMsg,
);

@Native<FreeC>(assetId: _kAssetId, symbol: 'dcb_free')
external void _dcbFree(Pointer<Void> p);

@Native<SetVerboseErrorsC>(assetId: _kAssetId, symbol: 'dcb_set_verbose_errors')
external void _dcbSetVerboseErrors(int enabled);

@Native<SetPoolThreadsC>(assetId: _kAssetId, symbol: 'dcb_set_pool_threads')
external void _dcbSetPoolThreads(int n);

@Native<SetIoThreadsC>(assetId: _kAssetId, symbol: 'dcb_set_io_threads')
external void _dcbSetIoThreads(int n);

@Native<Pointer<Void> Function()>(
  assetId: _kAssetId,
  symbol: 'dcb_session_finalizer_ptr',
)
external Pointer<Void> _dcbSessionFinalizerPtr();

@Native<Pointer<Void> Function()>(
  assetId: _kAssetId,
  symbol: 'dcb_drop_object_ptr',
)
external Pointer<Void> _dcbDropObjectPtr();

@Native<ObjectFinalizerTokenC>(
  assetId: _kAssetId,
  symbol: 'dcb_object_finalizer_token',
)
external Pointer<Void> _dcbObjectFinalizerToken(int handle);

@Native<Pointer<Void> Function()>(
  assetId: _kAssetId,
  symbol: 'dcb_object_finalizer_ptr',
)
external Pointer<Void> _dcbObjectFinalizerPtr();

/// Creates the [NativeBindings] for this package\'s native library.
NativeBindings createDcbBindings() {{
  return NativeBindings(
    initDartApi: _dcbInitDartApi,
    sessionOpen: _dcbSessionOpen,
    sessionClose: _dcbSessionClose,
    sessionFinalizer: _dcbSessionFinalizerPtr().cast<FinalizerFn>(),
    shutdown: _dcbShutdown,
    invokeSync: _dcbInvokeSync,
    invokeAsync: _dcbInvokeAsync,
    streamClose: _dcbStreamClose,
    dartFnReply: _dcbDartFnReply,
    free: _dcbFree,
    setVerboseErrors: _dcbSetVerboseErrors,
    setPoolThreads: _dcbSetPoolThreads,
    setIoThreads: _dcbSetIoThreads,
    dropObject: _dcbDropObjectPtr().cast<FinalizerFn>(),
    objectFinalizer: _dcbObjectFinalizerPtr().cast<FinalizerFn>(),
    objectFinalizerToken: _dcbObjectFinalizerToken,
  );
}}
'''


def generate_dart_init(
    *,
    lib_class: str = "DcbLib",
    impl_class: str = "BridgeApiImpl",
    impl_import: str = "../dcb_generated.dart",
) -> str:
    """Generate api/init.dart — DcbLib management class."""
    return f"""// GENERATED by dart_cpp_bridge codegen — do not edit.

import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';

import '../dcb_bindings.dart';
import '{impl_import}';

/// dart_cpp_bridge management class.
///
/// Usage:
/// ```dart
/// await {lib_class}.init();
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
  /// [threadPoolSize] sets the native blocking thread pool concurrency (default 4).
  /// [ioThreads] sets the io scheduler runner count (default 1).
  /// [verboseErrors] controls whether C++ errors include function names (default true).
  static Future<void> init({{int threadPoolSize = 4, int ioThreads = 1, bool verboseErrors = true}}) async {{
    if (_bridge != null) return;
    final b = await DartCppBridge.init(
      bindings: createDcbBindings(),
      poolThreads: threadPoolSize,
      ioThreads: ioThreads,
    );
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
    dart_code: dict[str, str] | None = None,
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

    # Map data_class qualified name -> defining header (cross-header imports).
    data_class_headers: dict[str, str] = {}
    for cls in ir.get("classes", []):
        if cls.get("kind") == "data_class":
            data_class_headers[cls["qualified"]] = cls.get("header", "")

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

        # Enums stay in dcb_generated.dart; re-export them for convenience.
        export_names: list[str] = [e["name"] for e in enums]

        # Data classes are defined directly in this API file (dependency order).
        data_classes = _order_data_classes(
            [c for c in classes if c.get("kind") == "data_class"]
        )
        local_dc_quals = {c["qualified"] for c in data_classes}

        # Cross-header data class references (function signatures + local fields).
        ref_quals: set[str] = set()
        for fn in fns:
            ref_quals |= _data_class_type_quals(fn["return"])
            for a in fn["args"]:
                ref_quals |= _data_class_type_quals(a["type"])
        for cls in data_classes:
            for f in cls.get("fields", []):
                ref_quals |= _data_class_type_quals(f["type"])
        ext_by_file: dict[str, list[str]] = {}
        for q in sorted(ref_quals - local_dc_quals):
            h = data_class_headers.get(q, "")
            if not h or h == header:
                continue
            hp2 = PureWindowsPath(h) if "\\" in h else PurePosixPath(h)
            ext_by_file.setdefault(hp2.stem + ".dart", []).append(q.rsplit("::", 1)[-1])

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
        ) or any(
            _is_optional_sink_arg(a)
            for cls in classes for m in cls.get("methods", []) for a in m["args"]
        )
        has_u8_ptr = any(
            _fn_or_method_uses_u8_ptr(fn) for fn in fns
        ) or any(
            _fn_or_method_uses_u8_ptr(m)
            for cls in classes for m in cls.get("methods", [])
        ) or any(
            _type_has_u8_ptr(f["type"])
            for cls in classes for f in cls.get("fields", [])
        )
        has_u8vec = any(
            _fn_or_method_uses_u8vec(fn) for fn in fns
        ) or any(
            _fn_or_method_uses_u8vec(m)
            for cls in classes for m in cls.get("methods", [])
        ) or any(
            _type_has_u8vec(f["type"])
            for cls in classes for f in cls.get("fields", [])
        )

        lines: list[str] = []
        lines.append("// GENERATED by dart_cpp_bridge codegen — do not edit.")
        lines.append(f"// Source: {hp.name}")
        lines.append("")
        if has_u8_ptr:
            lines.append("import 'dart:ffi';")
            lines.append("")
        if has_u8vec:
            lines.append("import 'dart:typed_data';")
            lines.append("")
        if has_dart_fn or has_opt_sink:
            lines.append("import 'dart:async';")
            lines.append("")
        if has_opaque:
            lines.append("import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';")
            lines.append("")
        if fns or has_opaque:
            lines.append("import '../dcb_generated.dart';")
            lines.append("")
        for fname, names in sorted(ext_by_file.items()):
            lines.append(f"import '{fname}' show {', '.join(sorted(names))};")
        if ext_by_file:
            lines.append("")
        if export_names:
            lines.append(f"export '../dcb_generated.dart' show {', '.join(sorted(export_names))};")
            lines.append("")

        # Generate data class definitions (they live here, not in dcb_generated).
        if data_classes:
            lines.append("// " + "═" * 45)
            lines.append("// Data classes")
            lines.append("// " + "═" * 45)
            lines.append("")
            lines.append(_dart_data_class_defs(data_classes, custom_code=dart_code))
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

    # Instance methods (the designated toString method is handled separately).
    instance_methods = [
        m for m in cls.get("methods", [])
        if m["kind"] != "constructor"
        and not m.get("is_static", False)
        and not m.get("to_string", False)
    ]
    if instance_methods:
        lines.append("  // ── Instance Methods ──")
        lines.append("")
        for method in instance_methods:
            lines.extend(_dart_opaque_method_lines(cls, method, is_static=False,
                                                   impl_class=impl_class, lib_class=lib_class))
            lines.append("")

    # Designated toString (BRIDGE_TO_STRING): forward to the sync wire method.
    to_string_method = next(
        (m for m in cls.get("methods", []) if m.get("to_string", False)), None
    )
    if to_string_method:
        impl_name = _class_impl_method_name(cls, to_string_method)
        lines.append("  @override")
        lines.append(f"  String toString() => {impl_class}.instance.{impl_name}(this);")
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


def _resolve_package_name(raw: dict[str, Any], project_root: Path) -> str:
    """Resolve the Dart package name from config or pubspec.yaml."""
    if "dart_package" in raw:
        return str(raw["dart_package"])
    pubspec = project_root / "pubspec.yaml"
    if pubspec.exists():
        from config_util import load_yaml
        ps = load_yaml(pubspec)
        name = ps.get("name")
        if name:
            return str(name)
    raise ValueError(
        "Cannot determine Dart package name. Add 'dart_package: <name>' to dart_cpp_bridge.yaml "
        "or ensure pubspec.yaml exists in the project root."
    )


def run_generate(config_path: Path) -> dict[str, Any]:
    result = parse_project(config_path)
    cfg = result["cfg"]
    ir = result["ir"]
    raw = cfg.get("raw") or {}

    impl_class = str(raw.get("dart_impl_class", "BridgeApiImpl"))
    lib_class = str(raw.get("dart_lib_class", "DcbLib"))
    impl_file = str(raw.get("dart_impl_file", "dcb_generated.dart"))
    api_subdir = str(raw.get("dart_api_subdir", "api"))
    package_name = _resolve_package_name(raw, cfg["project_root"])

    # Optional per-type custom Dart code injected into data class bodies
    # (e.g. a custom toString). Keys are data class names.
    dart_code_cfg = raw.get("dart_code") or {}
    if not isinstance(dart_code_cfg, dict):
        dart_code_cfg = {}

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
    dart_bindings = generate_dart_bindings(package_name)
    dart_init = generate_dart_init(
        lib_class=lib_class,
        impl_class=impl_class,
        impl_import=f"../{impl_file}",
    )
    api_files = generate_dart_api_files(
        ir,
        impl_class=impl_class,
        lib_class=lib_class,
        dart_code={str(k): str(v) for k, v in dart_code_cfg.items()},
    )

    cpp_out: Path = cfg["cpp_wire_output"]
    dart_out: Path = cfg["dart_output"]
    api_out: Path = dart_out / api_subdir
    cpp_out.mkdir(parents=True, exist_ok=True)
    dart_out.mkdir(parents=True, exist_ok=True)
    api_out.mkdir(parents=True, exist_ok=True)

    def _strip_ws(text: str) -> str:
        # Generated templates may leave blank lines with trailing indentation
        # (e.g. an empty arg_reads block); keep the output clean for
        # git diff --check and formatter hygiene.
        return "\n".join(line.rstrip() for line in text.split("\n"))

    (cpp_out / "wire_dispatch.hpp").write_text(_strip_ws(hpp), encoding="utf-8")
    (cpp_out / "wire_dispatch.cpp").write_text(_strip_ws(cpp), encoding="utf-8")
    (cpp_out / "ir.json").write_text(json.dumps(ir, indent=2) + "\n", encoding="utf-8")
    (dart_out / impl_file).write_text(_strip_ws(dart_impl), encoding="utf-8")
    (dart_out / "dcb_bindings.dart").write_text(_strip_ws(dart_bindings), encoding="utf-8")
    (api_out / "init.dart").write_text(_strip_ws(dart_init), encoding="utf-8")
    for filename, content in api_files.items():
        (api_out / filename).write_text(_strip_ws(content), encoding="utf-8")

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
    print(f"  Dart bind   -> {info['dart_out']}/dcb_bindings.dart")
    print(f"  Dart init   -> {info['dart_out']}/{info['api_subdir']}/init.dart")
    for f in info["api_files"]:
        print(f"  Dart api    -> {info['dart_out']}/{info['api_subdir']}/{f}")
    for fn in info["ir"]["functions"]:
        print(f"  - {fn['kind']:6} id={fn['method_id']} {fn['qualified']}")
    return 0 if info["functions"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
