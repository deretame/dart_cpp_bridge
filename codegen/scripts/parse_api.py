"""Scan configured headers and collect BRIDGE_*/StreamSink-marked APIs into IR."""

from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any

from clang.cindex import Config, CursorKind, Index, TranslationUnit

# Ensure scripts dir import
_SCRIPTS = Path(__file__).resolve().parent
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

from config_util import resolve_config  # noqa: E402

ATTR_SYNC = "bridge::sync"
ATTR_ASYNC = "bridge::async"
ATTR_NORMAL = "bridge::normal"
ATTR_EXPORT = "bridge::export"


def _stable_method_id(qualified: str) -> int:
    # Keep away from Phase-1 demo ids 1..9
    h = hashlib.sha256(qualified.encode("utf-8")).digest()
    v = int.from_bytes(h[:4], "little") & 0x7FFFFFFF
    if v < 1000:
        v += 1000
    return v


def _cursor_attrs(cursor) -> set[str]:
    """Collect bridge::* markers from annotate attributes (see annotate.h)."""
    attrs: set[str] = set()

    def consider(s: str) -> None:
        s = (s or "").strip().strip('"')
        if not s:
            return
        m = re.search(r"bridge::\w+", s)
        if m:
            attrs.add(m.group(0))

    for ch in cursor.get_children():
        kn = ch.kind.name if hasattr(ch.kind, "name") else str(ch.kind)
        # ANNOTATE_ATTR spelling is typically the annotate string
        if "ATTR" in kn or ch.kind == CursorKind.ANNOTATE_ATTR:
            consider(ch.spelling)
            consider(ch.displayname)
        consider(ch.spelling)
        consider(ch.displayname)
    return attrs


def _enum_constant_name_to_dart(name: str) -> str:
    """Convert common C++ enum value naming to Dart camelCase.

    Examples:
      kOk        -> ok
      k_not_found -> notFound
      server_error -> serverError
    """
    if name.startswith("k_"):
        name = name[2:]
    elif name.startswith("k") and len(name) > 1 and name[1].isupper():
        name = name[1:]
    parts = name.split("_")
    if not parts:
        return name
    return parts[0][0].lower() + parts[0][1:] + "".join(p.title() for p in parts[1:])


def _collect_enums(tu, header_path: Path) -> list[dict[str, Any]]:
    """Collect enum declarations from a translation unit."""
    out: list[dict[str, Any]] = []
    header_s = str(header_path.resolve())

    def visit(cursor, ns_stack: list[str]):
        if cursor.kind == CursorKind.NAMESPACE:
            name = cursor.spelling or ""
            for ch in cursor.get_children():
                visit(ch, ns_stack + ([name] if name else []))
            return

        if cursor.kind == CursorKind.ENUM_DECL:
            loc = cursor.location
            if not loc.file:
                return
            if Path(loc.file.name).resolve() != Path(header_s):
                return
            qname = "::".join([n for n in ns_stack if n] + [cursor.spelling])
            values = []
            for ch in cursor.get_children():
                if ch.kind == CursorKind.ENUM_CONSTANT_DECL:
                    values.append(
                        {
                            "name": ch.spelling,
                            "dart_name": _enum_constant_name_to_dart(ch.spelling),
                            "value": ch.enum_value,
                        }
                    )
            # underlying type: prefer enum's integer type, else int32_t
            underlying = cursor.enum_type.spelling if cursor.enum_type else "std::int32_t"
            out.append(
                {
                    "name": cursor.spelling,
                    "qualified": qname,
                    "underlying": underlying,
                    "values": values,
                    "header": header_s,
                }
            )
            return

        for ch in cursor.get_children():
            visit(ch, ns_stack)

    visit(tu.cursor, [])
    return out


def _type_ir(
    type_spell: str,
    enum_by_qualified: dict[str, dict[str, Any]] | None = None,
    enum_by_name: dict[str, dict[str, Any]] | None = None,
) -> dict[str, Any]:
    s = " ".join(type_spell.replace("&&", " ").replace("&", " ").split())
    # strip const/class/struct
    s = re.sub(r"\b(const|volatile|class|struct)\b", "", s)
    s = " ".join(s.split())

    m = re.match(r"async_simple::coro::Lazy\s*<\s*(.+)\s*>$", s)
    if m:
        inner = _type_ir(m.group(1).strip(), enum_by_qualified, enum_by_name)
        return {"kind": "lazy", "inner": inner}

    m = re.match(r"StreamSink\s*<\s*(.+)\s*>$", s) or re.match(
        r"dcb::StreamSink\s*<\s*(.+)\s*>$", s
    )
    if m:
        return {
            "kind": "stream_sink",
            "inner": _type_ir(m.group(1).strip(), enum_by_qualified, enum_by_name),
        }

    m = re.match(r"std::optional\s*<\s*(.+)\s*>\s*$", s)
    if m:
        inner = _type_ir(m.group(1).strip(), enum_by_qualified, enum_by_name)
        return {"kind": "optional", "inner": inner}

    # enum references
    if enum_by_qualified and s in enum_by_qualified:
        e = enum_by_qualified[s]
        return {"kind": "enum", "name": e["name"], "qualified": e["qualified"]}
    if enum_by_name and s in enum_by_name:
        e = enum_by_name[s]
        return {"kind": "enum", "name": e["name"], "qualified": e["qualified"]}
    # bare unqual name like "StatusCode" when no namespace in spelling
    name = s.split("::")[-1]
    if enum_by_name and name in enum_by_name:
        e = enum_by_name[name]
        return {"kind": "enum", "name": e["name"], "qualified": e["qualified"]}

    aliases = {
        "int": "i32",
        "int32_t": "i32",
        "std::int32_t": "i32",
        "uint32_t": "u32",
        "std::uint32_t": "u32",
        "int64_t": "i64",
        "std::int64_t": "i64",
        "bool": "bool",
        "void": "void",
        "std::string": "string",
        "string": "string",
    }
    if s in aliases:
        return {"kind": aliases[s]}
    # bare unqual
    if s.endswith("int32_t"):
        return {"kind": "i32"}
    return {"kind": "unsupported", "spelling": type_spell}


def _has_stream_sink(args: list[dict[str, Any]]) -> bool:
    return any(a["type"].get("kind") == "stream_sink" for a in args)


def _classify(attrs: set[str], ret: dict[str, Any], args: list[dict[str, Any]]) -> str | None:
    if _has_stream_sink(args):
        return "stream"
    if ATTR_SYNC in attrs:
        return "sync"
    if ATTR_ASYNC in attrs or ret.get("kind") == "lazy":
        return "async"
    if ATTR_NORMAL in attrs:
        return "normal"
    return None


def _collect_functions(
    tu,
    header_path: Path,
    enum_by_qualified: dict[str, dict[str, Any]],
    enum_by_name: dict[str, dict[str, Any]],
) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    header_s = str(header_path.resolve())

    def visit(cursor, ns_stack: list[str]):
        if cursor.kind == CursorKind.NAMESPACE:
            name = cursor.spelling or ""
            for ch in cursor.get_children():
                visit(ch, ns_stack + ([name] if name else []))
            return

        if cursor.kind in (
            CursorKind.FUNCTION_DECL,
            CursorKind.CXX_METHOD,
            CursorKind.FUNCTION_TEMPLATE,
        ):
            loc = cursor.location
            if not loc.file:
                return
            if Path(loc.file.name).resolve() != Path(header_s):
                return
            # only definitions/declarations in this header
            attrs = _cursor_attrs(cursor)
            args = []
            for ch in cursor.get_children():
                if ch.kind == CursorKind.PARM_DECL:
                    args.append(
                        {
                            "name": ch.spelling or f"arg{len(args)}",
                            "type": _type_ir(
                                ch.type.spelling, enum_by_qualified, enum_by_name
                            ),
                        }
                    )
            ret = _type_ir(cursor.result_type.spelling, enum_by_qualified, enum_by_name)
            kind = _classify(attrs, ret, args)
            if kind is None:
                return
            # unwrap Lazy for return payload type
            ret_payload = ret["inner"] if ret.get("kind") == "lazy" else ret
            qname = "::".join([n for n in ns_stack if n] + [cursor.spelling])
            out.append(
                {
                    "name": cursor.spelling,
                    "qualified": qname,
                    "kind": kind,
                    "method_id": _stable_method_id(qname),
                    "args": args,
                    "return": ret_payload,
                    "attrs": sorted(attrs),
                    "header": header_s,
                }
            )
            return

        for ch in cursor.get_children():
            visit(ch, ns_stack)

    visit(tu.cursor, [])
    return out


def collect_headers(scan_dirs: list[Path]) -> list[Path]:
    headers: list[Path] = []
    for d in scan_dirs:
        if not d.is_dir():
            raise FileNotFoundError(f"scan dir not found: {d}")
        for p in sorted(d.rglob("*")):
            if p.suffix.lower() in (".h", ".hpp") and p.is_file():
                headers.append(p.resolve())
    return headers


def parse_project(config_path: Path) -> dict[str, Any]:
    cfg = resolve_config(config_path)
    headers = collect_headers(cfg["scan"])
    if not headers:
        raise RuntimeError(f"no headers under scan={cfg['scan']}")

    args = [f"-std={cfg['std']}", "-x", "c++"]
    for d in cfg["defines"]:
        args.append(f"-D{d}")
    for inc in cfg["include_paths"]:
        args.append(f"-I{inc}")

    index = Index.create()
    diags_out: list[str] = []

    # Pass 1: collect enum declarations across all headers.
    enums: list[dict[str, Any]] = []
    for h in headers:
        tu = index.parse(
            str(h),
            args=args,
            options=TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD
            | TranslationUnit.PARSE_INCOMPLETE,
        )
        if tu is None:
            raise RuntimeError(f"parse failed: {h}")
        for d in tu.diagnostics:
            if d.severity >= 3:
                diags_out.append(f"{h}: {d.spelling}")
        enums.extend(_collect_enums(tu, h))

    enum_by_qualified = {e["qualified"]: e for e in enums}
    # If multiple enums share a short name, the last one wins; codegen will
    # rely on qualified spelling in function signatures when available.
    enum_by_name: dict[str, dict[str, Any]] = {}
    for e in enums:
        enum_by_name[e["name"]] = e

    # Pass 2: collect marked functions with enum-aware type resolution.
    functions: list[dict[str, Any]] = []
    for h in headers:
        tu = index.parse(
            str(h),
            args=args,
            options=TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD
            | TranslationUnit.PARSE_INCOMPLETE,
        )
        if tu is None:
            raise RuntimeError(f"parse failed: {h}")
        for d in tu.diagnostics:
            if d.severity >= 3:
                diags_out.append(f"{h}: {d.spelling}")
        functions.extend(_collect_functions(tu, h, enum_by_qualified, enum_by_name))

    # de-dupe by qualified name
    by_q: dict[str, dict[str, Any]] = {}
    for fn in functions:
        by_q[fn["qualified"]] = fn

    ir = {
        "version": 1,
        "config": str(cfg["config_path"]),
        "headers": [str(h) for h in headers],
        "clang_args": args,
        "enums": sorted(enums, key=lambda e: e["qualified"]),
        "functions": sorted(by_q.values(), key=lambda f: f["method_id"]),
        "diagnostics": diags_out,
    }
    return {"cfg": cfg, "ir": ir}


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv:
        print("usage: parse_api.py <dart_cpp_bridge.yaml> [--out ir.json]", file=sys.stderr)
        return 2
    config_path = Path(argv[0])
    out_path = None
    if "--out" in argv:
        out_path = Path(argv[argv.index("--out") + 1])

    result = parse_project(config_path)
    ir = result["ir"]
    text = json.dumps(ir, indent=2)
    if out_path:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(text + "\n", encoding="utf-8")
        print(f"wrote {out_path} ({len(ir['functions'])} functions)")
    else:
        print(text)

    if ir["diagnostics"]:
        print("parse diagnostics (errors):", file=sys.stderr)
        for d in ir["diagnostics"]:
            print(" ", d, file=sys.stderr)
        # still allow incomplete parse for stubs
    if not ir["functions"]:
        print("WARNING: no marked functions found", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
