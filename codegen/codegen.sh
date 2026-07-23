#!/bin/bash
# Codegen entry (Unix). Bootstraps pinned toolchain, then runs a Python script with it.
set -euo pipefail

CODEGEN_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$CODEGEN_ROOT"

FORCE_ARGS=()
SCRIPT="scripts/smoke_toolchain.py"
SCRIPT_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --force)
      FORCE_ARGS+=(--force)
      shift
      ;;
    -s|--script)
      SCRIPT="$2"
      shift 2
      ;;
    --)
      shift
      SCRIPT_ARGS+=("$@")
      break
      ;;
    *)
      if [[ "$1" == *.py || "$1" == */* ]]; then
        SCRIPT="$1"
        shift
        SCRIPT_ARGS+=("$@")
        break
      else
        SCRIPT_ARGS+=("$1")
        shift
      fi
      ;;
  esac
done

BOOT_LOG="$(mktemp)"
# Use the same bash interpreter that runs this script to avoid picking up a
# different bash (e.g. WSL /c/Windows/system32/bash) from PATH.
if ! "${BASH}" "${CODEGEN_ROOT}/bootstrap.sh" ${FORCE_ARGS[@]+"${FORCE_ARGS[@]}"} | tee "$BOOT_LOG"; then
  rm -f "$BOOT_LOG"
  exit 1
fi
# last non-empty line is python exe
PY="$(grep -E . "$BOOT_LOG" | tail -n 1)"
rm -f "$BOOT_LOG"

[[ -n "$PY" && -e "$PY" ]] || { echo "pinned python not found from bootstrap" >&2; exit 1; }

if [[ "$SCRIPT" != /* ]]; then
  SCRIPT_PATH="${CODEGEN_ROOT}/${SCRIPT}"
else
  SCRIPT_PATH="$SCRIPT"
fi
[[ -f "$SCRIPT_PATH" ]] || { echo "script not found: $SCRIPT_PATH" >&2; exit 1; }

echo ">> $PY $SCRIPT_PATH ${SCRIPT_ARGS[*]:-}"
exec "$PY" "$SCRIPT_PATH" ${SCRIPT_ARGS[@]+"${SCRIPT_ARGS[@]}"}
