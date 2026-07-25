"""Config loader for dart_cpp_bridge.yaml using ruamel.yaml."""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any


def load_yaml(path: Path) -> dict[str, Any]:
    """Load a YAML file using ruamel.yaml (safe mode)."""
    try:
        from ruamel.yaml import YAML
    except ImportError:
        print(
            "error: ruamel.yaml is not installed in the codegen toolchain.\n"
            "Run bootstrap first: codegen/bootstrap.ps1 (or bootstrap.sh)",
            file=sys.stderr,
        )
        raise SystemExit(1)

    yaml = YAML(typ="safe")
    yaml.preserve_quotes = True  # type: ignore[assignment]
    text = path.read_text(encoding="utf-8")
    try:
        data = yaml.load(text)
    except Exception as e:
        print(f"error: failed to parse YAML file {path}: {e}", file=sys.stderr)
        raise SystemExit(1)
    if data is None:
        data = {}
    if not isinstance(data, dict):
        print(f"error: {path}: top-level YAML must be a mapping", file=sys.stderr)
        raise SystemExit(1)
    return dict(data)


def resolve_config(config_path: Path) -> dict[str, Any]:
    cfg = load_yaml(config_path)
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
