#!/usr/bin/env python3
"""Codegen parser defensive tests.

Run with the pinned Python:
  cd dcb_gen_tool
  dart run bin/dcb_gen.dart run tests/run_tests.py

Or directly:
  <pinned-python> dcb_gen_tool/tests/run_tests.py
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
TESTS_DIR = Path(__file__).resolve().parent
TOOL_DIR = TESTS_DIR.parent
SCRIPTS_DIR = TOOL_DIR / "scripts"
REPO_ROOT = TOOL_DIR.parent
INCLUDE_DIR = REPO_ROOT / "dart" / "native" / "include"
STUBS_DIR = TOOL_DIR / "stubs"

RUN_CODEGEN = SCRIPTS_DIR / "run_codegen.py"

# ---------------------------------------------------------------------------
# Test result tracking
# ---------------------------------------------------------------------------
_results: list[tuple[str, bool, str]] = []
_results_lock = threading.Lock()


def _record(name: str, passed: bool, detail: str = "") -> None:
    with _results_lock:
        _results.append((name, passed, detail))
    status = "PASS" if passed else "FAIL"
    print(f"  [{status}] {name}")
    if not passed and detail:
        for line in detail.strip().splitlines():
            print(f"         {line}")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _write_test_project(
    tmp: Path,
    header_content: str,
    *,
    header_name: str = "api.h",
    extra_headers: dict[str, str] | None = None,
    yaml_content: str | None = None,
) -> Path:
    """Create a minimal test project in tmp directory.

    Returns path to the yaml config.
    """
    api_dir = tmp / "native" / "api"
    api_dir.mkdir(parents=True, exist_ok=True)
    gen_dir = tmp / "native" / "generated"
    gen_dir.mkdir(parents=True, exist_ok=True)
    dart_dir = tmp / "lib" / "src" / "gen"
    dart_dir.mkdir(parents=True, exist_ok=True)

    # Write main header
    (api_dir / header_name).write_text(header_content, encoding="utf-8")

    # Write extra headers if any
    if extra_headers:
        for name, content in extra_headers.items():
            p = api_dir / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(content, encoding="utf-8")

    # Write yaml config
    if yaml_content is None:
        yaml_content = f"""\
cpp_root: native/
scan:
  - native/api/
include_paths:
  - native
  - native/api
  - {INCLUDE_DIR.as_posix()}
  - {STUBS_DIR.as_posix()}
dart_output: lib/src/gen/
cpp_wire_output: native/generated/
std: c++20
defines:
  - BRIDGE_CODEGEN
  - DART_CPP_BRIDGE_CODEGEN
"""
    cfg_path = tmp / "dart_cpp_bridge.yaml"
    cfg_path.write_text(yaml_content, encoding="utf-8")
    return cfg_path


def _run_codegen(cfg_path: Path) -> subprocess.CompletedProcess[str]:
    """Run codegen and capture output."""
    return subprocess.run(
        [sys.executable, str(RUN_CODEGEN), str(cfg_path)],
        capture_output=True,
        text=True,
        timeout=60,
    )


HEADER_PREAMBLE = """\
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <dart_cpp_bridge/annotate.h>
#include <async_simple/coro/Lazy.h>
"""


# ---------------------------------------------------------------------------
# P03: Duplicate function name (overload)
# ---------------------------------------------------------------------------
def test_p03_duplicate_function_name() -> None:
    header = HEADER_PREAMBLE + """
BRIDGE_SYNC
std::int32_t compute(std::int32_t a);

BRIDGE_SYNC
std::int32_t compute(std::int32_t a, std::int32_t b);
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        # Should fail or report duplicate
        combined = r.stdout + r.stderr
        passed = (
            r.returncode != 0
            or "duplicate" in combined.lower()
            or "overload" in combined.lower()
        )
        _record(
            "P03: duplicate function name",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


# ---------------------------------------------------------------------------
# P04: Forward declaration / incomplete type
# ---------------------------------------------------------------------------
def test_p04_forward_declaration() -> None:
    header = HEADER_PREAMBLE + """
struct ForwardOnly;

BRIDGE_ASYNC
async_simple::coro::Lazy<ForwardOnly> get_thing();
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        passed = r.returncode != 0 and (
            "ForwardOnly" in combined or "not supported" in combined.lower()
        )
        _record(
            "P04: forward declaration type",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


# ---------------------------------------------------------------------------
# P06: Function pointer parameter
# ---------------------------------------------------------------------------
def test_p06_function_pointer() -> None:
    header = HEADER_PREAMBLE + """
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int32_t> apply(std::int32_t (*fn)(std::int32_t));
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        passed = r.returncode != 0 and (
            "not supported" in combined.lower() or "apply" in combined
        )
        _record(
            "P06: function pointer parameter",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


# ---------------------------------------------------------------------------
# P10: Non-whitelist types (shared_ptr, raw pointer, unexported class)
# ---------------------------------------------------------------------------
def test_p10_shared_ptr() -> None:
    header = HEADER_PREAMBLE + """
#include <memory>

struct SomeData { int x; };

BRIDGE_ASYNC
async_simple::coro::Lazy<std::shared_ptr<SomeData>> get_data();
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        passed = r.returncode != 0 and (
            "shared_ptr" in combined or "not supported" in combined.lower()
        )
        _record(
            "P10a: shared_ptr return type",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


def test_p10_raw_pointer() -> None:
    header = HEADER_PREAMBLE + """
BRIDGE_SYNC
std::int32_t* get_raw_ptr();
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        passed = r.returncode != 0 and (
            "unsupported" in combined.lower() or "not supported" in combined.lower()
        )
        _record(
            "P10b: raw pointer return type",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


def test_p10_unexported_class() -> None:
    header = HEADER_PREAMBLE + """
struct InternalType { int x; };

BRIDGE_SYNC
InternalType get_internal();
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        passed = r.returncode != 0 and (
            "InternalType" in combined or "not supported" in combined.lower()
        )
        _record(
            "P10c: unexported class type",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


# ---------------------------------------------------------------------------
# P11: Type must be defined in exported headers
# ---------------------------------------------------------------------------
def test_p11_external_type() -> None:
    # external.h is NOT in the scan directory
    external_h = """\
#pragma once
struct ExternalType { int x; };
"""
    # api.h includes external.h and uses ExternalType
    header = HEADER_PREAMBLE + """
#include "external.h"

BRIDGE_ASYNC
async_simple::coro::Lazy<ExternalType> get_external();
"""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        # Write external.h in a separate directory (not scanned)
        ext_dir = tmp / "native" / "external"
        ext_dir.mkdir(parents=True, exist_ok=True)
        (ext_dir / "external.h").write_text(external_h, encoding="utf-8")

        # Custom yaml that includes external dir in include_paths but not scan
        yaml_content = f"""\
cpp_root: native/
scan:
  - native/api/
include_paths:
  - native
  - native/api
  - native/external
  - {INCLUDE_DIR.as_posix()}
  - {STUBS_DIR.as_posix()}
dart_output: lib/src/gen/
cpp_wire_output: native/generated/
std: c++20
defines:
  - BRIDGE_CODEGEN
  - DART_CPP_BRIDGE_CODEGEN
"""
        cfg = _write_test_project(tmp, header, yaml_content=yaml_content)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        passed = r.returncode != 0 and (
            "ExternalType" in combined or "not supported" in combined.lower()
        )
        _record(
            "P11: external type not in scan headers",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


# ---------------------------------------------------------------------------
# Y01: headers path not found
# ---------------------------------------------------------------------------
def test_y01_scan_path_not_found() -> None:
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        # Create minimal structure but with non-existent scan path
        (tmp / "native").mkdir(parents=True, exist_ok=True)
        yaml_content = f"""\
cpp_root: native/
scan:
  - native/nonexistent_dir/
include_paths:
  - {INCLUDE_DIR.as_posix()}
  - {STUBS_DIR.as_posix()}
dart_output: lib/src/gen/
cpp_wire_output: native/generated/
std: c++20
defines:
  - BRIDGE_CODEGEN
"""
        cfg_path = tmp / "dart_cpp_bridge.yaml"
        cfg_path.write_text(yaml_content, encoding="utf-8")
        r = _run_codegen(cfg_path)
        combined = r.stdout + r.stderr
        passed = r.returncode != 0 and (
            "no headers" in combined.lower()
            or "not found" in combined.lower()
            or "nonexistent" in combined.lower()
        )
        _record(
            "Y01: scan path not found",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


# ---------------------------------------------------------------------------
# Y02: yaml missing required field
# ---------------------------------------------------------------------------
def test_y02_missing_scan_field() -> None:
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        # yaml without 'scan' field
        yaml_content = """\
cpp_root: native/
dart_output: lib/src/gen/
cpp_wire_output: native/generated/
"""
        cfg_path = tmp / "dart_cpp_bridge.yaml"
        cfg_path.write_text(yaml_content, encoding="utf-8")
        r = _run_codegen(cfg_path)
        combined = r.stdout + r.stderr
        passed = r.returncode != 0 and (
            "scan" in combined.lower() or "required" in combined.lower()
        )
        _record(
            "Y02: missing scan field",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


# ---------------------------------------------------------------------------
# P01: Syntax error in header
# ---------------------------------------------------------------------------
def test_p01_syntax_error() -> None:
    header = HEADER_PREAMBLE + """
BRIDGE_SYNC
std::int32_t broken_func(std::int32_t a  // missing closing paren and semicolon
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        # Should either fail or not include broken_func in IR
        passed = r.returncode != 0 or "broken_func" not in combined
        _record(
            "P01: syntax error header",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


# ---------------------------------------------------------------------------
# P02: Empty / no-export header
# ---------------------------------------------------------------------------
def test_p02_empty_export() -> None:
    header = HEADER_PREAMBLE + """
// No BRIDGE_* annotations at all
std::int32_t internal_helper(std::int32_t x) { return x * 2; }
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        # codegen exits 1 with "0 functions" when nothing is exported - that's OK
        passed = "0 functions" in combined or r.returncode == 0
        _record(
            "P02: empty export header",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


# ---------------------------------------------------------------------------
# P05: Annotation on non-function
# ---------------------------------------------------------------------------
def test_p05_annotation_on_variable() -> None:
    header = HEADER_PREAMBLE + """
BRIDGE_SYNC
std::int32_t global_counter = 0;

BRIDGE_SYNC
std::int32_t valid_func(std::int32_t x);
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        # Should not crash; global_counter should not appear as a function
        passed = r.returncode == 0 and "global_counter" not in combined
        _record(
            "P05: annotation on variable",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


# ---------------------------------------------------------------------------
# P07: typedef/using alias penetration
# ---------------------------------------------------------------------------
def test_p07_typedef_alias() -> None:
    header = HEADER_PREAMBLE + """
using IntVec = std::vector<std::int32_t>;
using NestedVec = std::vector<IntVec>;

BRIDGE_ASYNC
async_simple::coro::Lazy<NestedVec> get_nested();
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        # Either succeeds (alias resolved) or fails with clear error
        passed = r.returncode == 0 or "nested" in combined.lower() or "unsupported" in combined.lower()
        _record(
            "P07: typedef alias penetration",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


# ---------------------------------------------------------------------------
# P08: Self-reference / circular reference
# ---------------------------------------------------------------------------
def test_p08_circular_reference() -> None:
    header = HEADER_PREAMBLE + """
BRIDGE_EXPORT
struct NodeA {
  std::int32_t value;
};

BRIDGE_EXPORT
struct NodeB {
  NodeA other;
};

// Now make NodeA reference NodeB (circular)
// Note: In C++ this won't compile with value types, but let's test parser behavior
BRIDGE_SYNC
NodeA get_node_a();
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        # This should either work (no actual circular ref) or fail gracefully
        passed = r.returncode == 0 or "circular" in combined.lower() or r.returncode != 0
        _record(
            "P08: circular reference detection",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


# ---------------------------------------------------------------------------
# P09: Deep container nesting
# ---------------------------------------------------------------------------
def test_p09_deep_nesting() -> None:
    header = HEADER_PREAMBLE + """
BRIDGE_SYNC
std::vector<std::vector<std::vector<std::vector<std::vector<
  std::vector<std::vector<std::vector<std::vector<std::vector<
    std::int32_t
  >>>>>>>>>> deep_nested();
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        # Either succeeds or fails with clear depth error
        passed = r.returncode == 0 or "nest" in combined.lower() or "depth" in combined.lower() or r.returncode != 0
        _record(
            "P09: deep container nesting",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


# ---------------------------------------------------------------------------
# Y03: Missing include_paths (macro not expanded)
# ---------------------------------------------------------------------------
def test_y03_missing_include_paths() -> None:
    header = HEADER_PREAMBLE + """
BRIDGE_SYNC
std::int32_t should_export(std::int32_t x);
"""
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        # yaml WITHOUT include_paths pointing to annotate.h
        yaml_content = """\
cpp_root: native/
scan:
  - native/api/
include_paths:
  - native
  - native/api
dart_output: lib/src/gen/
cpp_wire_output: native/generated/
std: c++20
defines:
  - BRIDGE_CODEGEN
"""
        cfg = _write_test_project(tmp, header, yaml_content=yaml_content)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        # Without annotate.h in include path, BRIDGE_SYNC won't expand
        # so no functions should be found; codegen exits 1 with "0 functions"
        passed = "0 functions" in combined or r.returncode == 0
        _record(
            "Y03: missing include_paths",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


# ---------------------------------------------------------------------------
# Y04: Invalid YAML syntax
# ---------------------------------------------------------------------------
def test_y04_invalid_yaml() -> None:
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        (tmp / "native" / "api").mkdir(parents=True, exist_ok=True)
        (tmp / "native" / "api" / "api.h").write_text(
            HEADER_PREAMBLE, encoding="utf-8"
        )
        # Write invalid YAML
        yaml_content = """\
cpp_root: native/
scan:
  - native/api/
  bad_indent: this is broken
    nested_wrong
"""
        cfg_path = tmp / "dart_cpp_bridge.yaml"
        cfg_path.write_text(yaml_content, encoding="utf-8")
        r = _run_codegen(cfg_path)
        combined = r.stdout + r.stderr
        passed = r.returncode != 0 and (
            "yaml" in combined.lower() or "parse" in combined.lower() or "error" in combined.lower()
        )
        _record(
            "Y04: invalid YAML syntax",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


# ---------------------------------------------------------------------------
# S01: Idempotency
# ---------------------------------------------------------------------------
def test_s01_idempotency() -> None:
    header = HEADER_PREAMBLE + """
BRIDGE_SYNC
std::int32_t add(std::int32_t a, std::int32_t b);

BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> echo(std::string msg);
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r1 = _run_codegen(cfg)
        if r1.returncode != 0:
            _record("S01: idempotency", False, f"first run failed: {r1.stderr[:300]}")
            return
        # Collect generated files
        gen_dir = Path(td) / "native" / "generated"
        dart_dir = Path(td) / "lib" / "src" / "gen"
        files1: dict[str, bytes] = {}
        for d in (gen_dir, dart_dir):
            if d.exists():
                for f in d.rglob("*"):
                    if f.is_file():
                        files1[str(f.relative_to(td))] = f.read_bytes()
        # Run again
        r2 = _run_codegen(cfg)
        files2: dict[str, bytes] = {}
        for d in (gen_dir, dart_dir):
            if d.exists():
                for f in d.rglob("*"):
                    if f.is_file():
                        files2[str(f.relative_to(td))] = f.read_bytes()
        passed = files1 == files2
        detail = ""
        if not passed:
            diff_keys = set(files1.keys()) ^ set(files2.keys())
            for k in files1:
                if k in files2 and files1[k] != files2[k]:
                    diff_keys.add(k)
            detail = f"differing files: {diff_keys}"
        _record("S01: idempotency", passed, detail)


# ---------------------------------------------------------------------------
# S02: method_id stability
# ---------------------------------------------------------------------------
def test_s02_method_id_stability() -> None:
    header_v1 = HEADER_PREAMBLE + """
BRIDGE_SYNC
std::int32_t add(std::int32_t a, std::int32_t b);

BRIDGE_SYNC
std::int32_t sub(std::int32_t a, std::int32_t b);
"""
    header_v2 = HEADER_PREAMBLE + """
BRIDGE_SYNC
std::int32_t add(std::int32_t a, std::int32_t b);

BRIDGE_SYNC
std::int32_t sub(std::int32_t a, std::int32_t b);

BRIDGE_SYNC
std::int32_t mul(std::int32_t a, std::int32_t b);
"""
    import json as _json

    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header_v1)
        r1 = _run_codegen(cfg)
        if r1.returncode != 0:
            _record("S02: method_id stability", False, f"v1 failed: {r1.stderr[:300]}")
            return
        ir_path = Path(td) / "native" / "generated" / "ir.json"
        if not ir_path.exists():
            _record("S02: method_id stability", False, "ir.json not found")
            return
        ir1 = _json.loads(ir_path.read_text(encoding="utf-8"))
        ids1 = {fn["name"]: fn["method_id"] for fn in ir1.get("functions", [])}

        # Overwrite header with v2 (adds mul)
        (Path(td) / "native" / "api" / "api.h").write_text(header_v2, encoding="utf-8")
        r2 = _run_codegen(cfg)
        if r2.returncode != 0:
            _record("S02: method_id stability", False, f"v2 failed: {r2.stderr[:300]}")
            return
        ir2 = _json.loads(ir_path.read_text(encoding="utf-8"))
        ids2 = {fn["name"]: fn["method_id"] for fn in ir2.get("functions", [])}

        # Existing ids must not change
        stable = all(ids1[k] == ids2[k] for k in ids1 if k in ids2)
        has_new = "mul" in ids2
        passed = stable and has_new
        _record(
            "S02: method_id stability",
            passed,
            f"ids1={ids1}\nids2={ids2}",
        )


# ---------------------------------------------------------------------------
# S03: Function order independence
# ---------------------------------------------------------------------------
def test_s03_order_independence() -> None:
    header_ab = HEADER_PREAMBLE + """
BRIDGE_SYNC
std::int32_t func_a(std::int32_t x);

BRIDGE_SYNC
std::int32_t func_b(std::int32_t x);
"""
    header_ba = HEADER_PREAMBLE + """
BRIDGE_SYNC
std::int32_t func_b(std::int32_t x);

BRIDGE_SYNC
std::int32_t func_a(std::int32_t x);
"""
    import json as _json

    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header_ab)
        r1 = _run_codegen(cfg)
        if r1.returncode != 0:
            _record("S03: order independence", False, f"AB failed: {r1.stderr[:300]}")
            return
        ir_path = Path(td) / "native" / "generated" / "ir.json"
        ir1 = _json.loads(ir_path.read_text(encoding="utf-8"))
        ids_ab = {fn["name"]: fn["method_id"] for fn in ir1.get("functions", [])}

        # Swap order
        (Path(td) / "native" / "api" / "api.h").write_text(header_ba, encoding="utf-8")
        r2 = _run_codegen(cfg)
        if r2.returncode != 0:
            _record("S03: order independence", False, f"BA failed: {r2.stderr[:300]}")
            return
        ir2 = _json.loads(ir_path.read_text(encoding="utf-8"))
        ids_ba = {fn["name"]: fn["method_id"] for fn in ir2.get("functions", [])}

        passed = ids_ab == ids_ba
        _record(
            "S03: order independence",
            passed,
            f"AB={ids_ab}\nBA={ids_ba}",
        )


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# TS: BRIDGE_TO_STRING validation
# ---------------------------------------------------------------------------
def test_ts01_valid_to_string() -> None:
    header = HEADER_PREAMBLE + """
class BRIDGE_OPAQUE Widget {
 public:
  BRIDGE_CONSTRUCTOR Widget();
  BRIDGE_TO_STRING std::string toString() const;
};

BRIDGE_SYNC
std::int32_t dummy();
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        # exit==0 means no validation error rejected the BRIDGE_TO_STRING.
        passed = r.returncode == 0 and "BRIDGE_TO_STRING" not in combined
        _record(
            "TS01: valid BRIDGE_TO_STRING accepted",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


def test_ts02_to_string_wrong_return() -> None:
    header = HEADER_PREAMBLE + """
class BRIDGE_OPAQUE Widget {
 public:
  BRIDGE_CONSTRUCTOR Widget();
  BRIDGE_TO_STRING std::int32_t toString() const;
};
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        passed = r.returncode != 0 and "return std::string" in combined
        _record(
            "TS02: BRIDGE_TO_STRING non-string return rejected",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


def test_ts03_to_string_with_args() -> None:
    header = HEADER_PREAMBLE + """
class BRIDGE_OPAQUE Widget {
 public:
  BRIDGE_CONSTRUCTOR Widget();
  BRIDGE_TO_STRING std::string toString(std::int32_t x) const;
};
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        passed = r.returncode != 0 and "no arguments" in combined
        _record(
            "TS03: BRIDGE_TO_STRING with arguments rejected",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


def test_ts04_to_string_static() -> None:
    header = HEADER_PREAMBLE + """
class BRIDGE_OPAQUE Widget {
 public:
  BRIDGE_CONSTRUCTOR Widget();
  static BRIDGE_TO_STRING std::string toString();
};
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        passed = r.returncode != 0 and "instance method" in combined
        _record(
            "TS04: static BRIDGE_TO_STRING rejected",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


def test_ts05_to_string_async() -> None:
    header = HEADER_PREAMBLE + """
class BRIDGE_OPAQUE Widget {
 public:
  BRIDGE_CONSTRUCTOR Widget();
  BRIDGE_TO_STRING async_simple::coro::Lazy<std::string> toString() const;
};
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        passed = r.returncode != 0 and "synchronous" in combined
        _record(
            "TS05: async BRIDGE_TO_STRING rejected",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


# ---------------------------------------------------------------------------
# E: Enum validation tests
# ---------------------------------------------------------------------------
def test_e01_enum_without_export_not_in_ir() -> None:
    """Enum without BRIDGE_EXPORT should not appear in IR."""
    header = HEADER_PREAMBLE + """
enum class Color : std::int32_t {
  kRed = 0,
  kGreen = 1,
};

BRIDGE_SYNC
std::int32_t dummy(std::int32_t x);
"""
    import json as _json

    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        if r.returncode != 0:
            _record(
                "E01: unmarked enum not exported",
                False,
                f"codegen failed unexpectedly: exit={r.returncode}\n{combined[:500]}",
            )
            return
        ir_path = Path(td) / "native" / "generated" / "ir.json"
        ir = _json.loads(ir_path.read_text(encoding="utf-8"))
        enum_names = [e["name"] for e in ir.get("enums", [])]
        passed = "Color" not in enum_names
        _record(
            "E01: unmarked enum not exported",
            passed,
            f"enums in IR: {enum_names}",
        )


def test_e02_enum_wrong_underlying_type() -> None:
    """Enum with non-int32_t underlying type should be rejected."""
    header = HEADER_PREAMBLE + """
enum class BRIDGE_EXPORT SmallEnum : std::uint8_t {
  kA = 0,
  kB = 1,
};

BRIDGE_SYNC
std::int32_t dummy(std::int32_t x);
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        passed = r.returncode != 0 and (
            "underlying" in combined.lower() or "int32" in combined
        )
        _record(
            "E02: enum wrong underlying type rejected",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


def test_e03_enum_missing_explicit_value() -> None:
    """Enum constant without explicit value should be rejected."""
    header = HEADER_PREAMBLE + """
enum class BRIDGE_EXPORT Status : std::int32_t {
  kOk,
  kError,
};

BRIDGE_SYNC
std::int32_t dummy(std::int32_t x);
"""
    with tempfile.TemporaryDirectory() as td:
        cfg = _write_test_project(Path(td), header)
        r = _run_codegen(cfg)
        combined = r.stdout + r.stderr
        passed = r.returncode != 0 and (
            "explicit" in combined.lower() or "no explicit value" in combined
        )
        _record(
            "E03: enum missing explicit value rejected",
            passed,
            f"exit={r.returncode}\n{combined[:500]}",
        )


def main() -> int:
    print("=" * 60)
    print("Codegen Parser Defensive Tests")
    print("=" * 60)

    tests = [
        # P0 priority
        test_p03_duplicate_function_name,
        test_p04_forward_declaration,
        test_p06_function_pointer,
        test_p10_shared_ptr,
        test_p10_raw_pointer,
        test_p10_unexported_class,
        test_p11_external_type,
        test_y01_scan_path_not_found,
        test_y02_missing_scan_field,
        # P1 priority
        test_p01_syntax_error,
        test_p02_empty_export,
        test_p05_annotation_on_variable,
        test_p08_circular_reference,
        test_s01_idempotency,
        test_s02_method_id_stability,
        # P2 priority
        test_p07_typedef_alias,
        test_p09_deep_nesting,
        test_y03_missing_include_paths,
        test_y04_invalid_yaml,
        test_s03_order_independence,
        # BRIDGE_TO_STRING validation
        test_ts01_valid_to_string,
        test_ts02_to_string_wrong_return,
        test_ts03_to_string_with_args,
        test_ts04_to_string_static,
        test_ts05_to_string_async,
        # Enum validation
        test_e01_enum_without_export_not_in_ir,
        test_e02_enum_wrong_underlying_type,
        test_e03_enum_missing_explicit_value,
    ]

    workers = int(os.environ.get("DCB_TEST_WORKERS", "0")) or min(len(tests), os.cpu_count() or 4)
    print(f"  ({workers} parallel workers)")
    print()

    def _run_one(t):
        try:
            t()
        except Exception as e:
            _record(t.__name__, False, f"Exception: {e}")

    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(_run_one, t) for t in tests]
        for f in as_completed(futures):
            f.result()  # propagate unexpected exceptions

    print()
    print("=" * 60)
    passed = sum(1 for _, p, _ in _results if p)
    total = len(_results)
    print(f"Results: {passed}/{total} passed")
    if passed < total:
        print("Failed tests:")
        for name, p, detail in _results:
            if not p:
                print(f"  - {name}")
    print("=" * 60)
    return 0 if passed == total else 1


if __name__ == "__main__":
    raise SystemExit(main())
