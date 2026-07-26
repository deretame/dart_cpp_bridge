#!/usr/bin/env python3
"""Verify pinned toolchain: Python version + libclang-ng (clang.cindex)."""

from __future__ import annotations

import platform
import sys


def main() -> int:
    print("executable:", sys.executable)
    print("version:", sys.version.replace("\n", " "))
    print("platform:", platform.platform())

    ver = sys.version_info
    if (ver.major, ver.minor) != (3, 13):
        print(
            f"ERROR: expected Python 3.13.x, got {ver.major}.{ver.minor}",
            file=sys.stderr,
        )
        return 1

    from clang.cindex import Config, Index

    print("clang Config.library_file:", Config.library_file)
    print("clang Config.library_path:", Config.library_path)

    index = Index.create()
    src = "int dcb_smoke_add(int a, int b) { return a + b; }\n"
    tu = index.parse(
        "dcb_smoke.cpp",
        args=["-std=c++20", "-x", "c++"],
        unsaved_files=[("dcb_smoke.cpp", src)],
    )
    if not tu:
        print("ERROR: parse returned None", file=sys.stderr)
        return 1

    diags = list(tu.diagnostics)
    errors = [d for d in diags if d.severity >= 3]
    if errors:
        for d in errors:
            print("ERROR diag:", d.spelling, file=sys.stderr)
        return 1

    names = [c.spelling for c in tu.cursor.get_children() if c.spelling]
    print("top-level names:", names)
    if "dcb_smoke_add" not in names:
        print("ERROR: expected function dcb_smoke_add in AST", file=sys.stderr)
        return 1

    print("smoke_toolchain: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
