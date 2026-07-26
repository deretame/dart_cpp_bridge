"""Codegen entry: parse + generate for a user project config."""

from __future__ import annotations

import sys
from pathlib import Path

_SCRIPTS = Path(__file__).resolve().parent
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))

from generate import main as gen_main  # noqa: E402


def main() -> int:
    argv = sys.argv[1:]
    if not argv:
        print(
            "usage: run_codegen.py <path/to/dart_cpp_bridge.yaml>",
            file=sys.stderr,
        )
        return 2
    return gen_main(argv)


if __name__ == "__main__":
    raise SystemExit(main())
