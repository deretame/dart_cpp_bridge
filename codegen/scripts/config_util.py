"""Minimal config loader for dart_cpp_bridge.yaml (small subset, no PyYAML)."""

from __future__ import annotations

from pathlib import Path
from typing import Any


def _parse_scalar(raw: str) -> Any:
    s = raw.strip()
    if not s:
        return ""
    if (s.startswith('"') and s.endswith('"')) or (s.startswith("'") and s.endswith("'")):
        return s[1:-1]
    if s in ("true", "True"):
        return True
    if s in ("false", "False"):
        return False
    if s.isdigit() or (s.startswith("-") and s[1:].isdigit()):
        return int(s)
    return s


def load_simple_yaml(path: Path) -> dict[str, Any]:
    """Parse a tiny YAML subset: top-level keys, nested maps, and `- list` items."""
    return _load_simple_yaml_v2(path)


def _load_simple_yaml_v2(path: Path) -> dict[str, Any]:
    text = path.read_text(encoding="utf-8")
    root: dict[str, Any] = {}
    # stack: (indent, container)
    stack: list[tuple[int, Any]] = [(-1, root)]

    lines = text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        i += 1
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        indent = len(line) - len(line.lstrip(" "))
        content = line.strip()

        while len(stack) > 1 and indent <= stack[-1][0]:
            stack.pop()
        parent = stack[-1][1]

        if content.startswith("- "):
            if not isinstance(parent, list):
                raise ValueError(f"{path}: list item without list parent: {content}")
            parent.append(_parse_scalar(content[2:].strip().split("#", 1)[0]))
            continue

        if ":" not in content:
            raise ValueError(f"{path}: bad line: {content}")

        key, _, rest = content.partition(":")
        key = key.strip()
        rest = rest.strip().split("#", 1)[0].strip()

        if rest != "":
            if not isinstance(parent, dict):
                raise ValueError(f"{path}: key under non-dict")
            parent[key] = _parse_scalar(rest)
            continue

        # look ahead for list vs map
        j = i
        child_kind = "map"
        while j < len(lines):
            peek = lines[j]
            j += 1
            if not peek.strip() or peek.lstrip().startswith("#"):
                continue
            pindent = len(peek) - len(peek.lstrip(" "))
            if pindent <= indent:
                break
            child_kind = "list" if peek.strip().startswith("- ") else "map"
            break

        node: Any = [] if child_kind == "list" else {}
        if not isinstance(parent, dict):
            raise ValueError(f"{path}: nested under non-dict")
        parent[key] = node
        stack.append((indent, node))

    return root


def resolve_config(config_path: Path) -> dict[str, Any]:
    cfg = load_simple_yaml(config_path)
    base = config_path.parent.resolve()
    cpp_root = Path(cfg.get("cpp_root", "."))
    if not cpp_root.is_absolute():
        cpp_root = (base / cpp_root).resolve()

    def rel_list(key: str, default: list[str] | None = None) -> list[Path]:
        raw = cfg.get(key, default or [])
        if not isinstance(raw, list):
            raise ValueError(f"{key} must be a list")
        out: list[Path] = []
        for item in raw:
            p = Path(str(item))
            out.append(p if p.is_absolute() else (base / p).resolve())
        return out

    dart_out = Path(str(cfg.get("dart_output", "lib/src/native_gen")))
    if not dart_out.is_absolute():
        dart_out = (base / dart_out).resolve()
    cpp_out = Path(str(cfg.get("cpp_wire_output", "native/generated")))
    if not cpp_out.is_absolute():
        cpp_out = (base / cpp_out).resolve()

    includes = rel_list("include_paths", [])
    scan = rel_list("scan", [])
    if not scan:
        raise ValueError("scan: must list at least one directory")

    defines = cfg.get("defines", ["BRIDGE_CODEGEN", "DART_CPP_BRIDGE_CODEGEN"])
    if not isinstance(defines, list):
        defines = [str(defines)]
    std = str(cfg.get("std", "c++20"))

    return {
        "config_path": config_path.resolve(),
        "project_root": base,
        "cpp_root": cpp_root,
        "scan": scan,
        "include_paths": includes,
        "dart_output": dart_out,
        "cpp_wire_output": cpp_out,
        "defines": [str(d) for d in defines],
        "std": std,
        "raw": cfg,
    }
