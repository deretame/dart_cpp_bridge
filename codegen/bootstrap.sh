#!/usr/bin/env bash
# Download + verify pinned Python and libclang-ng into a user-level cache.
#
# Layout (default):
#   Linux:  ${XDG_CACHE_HOME:-~/.cache}/dart_cpp_bridge/toolchain
#   macOS:  ~/Library/Caches/dart_cpp_bridge/toolchain
#   downloads/<sha256>.tar.gz|.whl
#   envs/<platform>-<lockfp16>/python + READY.json
#
# Override: DCB_CODEGEN_CACHE
set -euo pipefail

FORCE=0
if [[ "${1:-}" == "--force" ]]; then
  FORCE=1
fi

CODEGEN_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOCK_PATH="${CODEGEN_ROOT}/versions.lock"

die() { echo "error: $*" >&2; exit 1; }

platform_key() {
  local os arch
  case "$(uname -s)" in
    Linux*) os=linux ;;
    Darwin*) os=macos ;;
    MINGW*|MSYS*|CYGWIN*) os=windows ;;
    *) die "unsupported OS: $(uname -s)" ;;
  esac
  case "$(uname -m)" in
    x86_64|amd64) arch=x86_64 ;;
    aarch64|arm64) arch=aarch64 ;;
    *) die "unsupported arch: $(uname -m)" ;;
  esac
  echo "${os}-${arch}"
}

default_cache_root() {
  if [[ -n "${DCB_CODEGEN_CACHE:-}" ]]; then
    echo "$DCB_CODEGEN_CACHE"
    return
  fi
  case "$(uname -s)" in
    Darwin*)
      echo "${HOME}/Library/Caches/dart_cpp_bridge/toolchain"
      ;;
    *)
      echo "${XDG_CACHE_HOME:-${HOME}/.cache}/dart_cpp_bridge/toolchain"
      ;;
  esac
}

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    die "need sha256sum or shasum"
  fi
}

sha256_text() {
  if command -v sha256sum >/dev/null 2>&1; then
    printf '%s' "$1" | sha256sum | awk '{print $1}'
  else
    printf '%s' "$1" | shasum -a 256 | awk '{print $1}'
  fi
}

download_verified() {
  local url="$1" expect="$2" dest="$3"
  mkdir -p "$(dirname "$dest")"
  if [[ -f "$dest" ]]; then
    local got
    got="$(sha256_file "$dest")"
    if [[ "$got" == "$expect" ]]; then
      echo "  cache hit: $(basename "$dest")"
      return
    fi
    echo "  hash mismatch, re-download: $(basename "$dest")"
    rm -f "$dest"
  fi
  echo "  downloading: $url"
  local tmp="${dest}.part"
  rm -f "$tmp"
  if command -v curl >/dev/null 2>&1; then
    curl -fL --retry 3 -o "$tmp" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget -O "$tmp" "$url"
  else
    die "need curl or wget"
  fi
  local got
  got="$(sha256_file "$tmp")"
  if [[ "$got" != "$expect" ]]; then
    rm -f "$tmp"
    die "SHA256 mismatch for $(basename "$dest") expected=$expect got=$got"
  fi
  mv "$tmp" "$dest"
  echo "  verified: $(basename "$dest")"
}

read_lock_field() {
  local path="$1"
  if command -v jq >/dev/null 2>&1; then
    jq -r "$path" "$LOCK_PATH"
    return
  fi
  if command -v python3 >/dev/null 2>&1; then
    python3 - "$LOCK_PATH" "$path" <<'PY'
import json, re, sys
lock = json.load(open(sys.argv[1], encoding="utf-8"))
s = sys.argv[2]
if s.startswith("."):
    s = s[1:]
cur = lock
for a, b in re.findall(r'(?:\[\"([^\"]+)\"\]|([^.\[\]]+))', s):
    cur = cur[a or b]
print(cur)
PY
    return
  fi
  die "need jq or python3 to parse versions.lock during bootstrap"
}

find_python_exe() {
  local root="$1" c
  for c in \
    "$root/bin/python3" \
    "$root/python/bin/python3" \
    "$root/bin/python" \
    "$root/python.exe" \
    "$root/python/python.exe"
  do
    if [[ -x "$c" || -f "$c" ]]; then
      echo "$c"
      return
    fi
  done
  die "python executable not found under $root"
}

[[ -f "$LOCK_PATH" ]] || die "missing versions.lock"

PLATFORM="$(platform_key)"
echo "platform: $PLATFORM"

PY_VER="$(read_lock_field '.python.version')"
PY_URL="$(read_lock_field ".python.platforms[\"${PLATFORM}\"].url")"
PY_SHA="$(read_lock_field ".python.platforms[\"${PLATFORM}\"].sha256" | tr 'A-F' 'a-f')"
LC_VER="$(read_lock_field '.libclang_ng.version')"
LC_URL="$(read_lock_field ".libclang_ng.platforms[\"${PLATFORM}\"].url")"
LC_SHA="$(read_lock_field ".libclang_ng.platforms[\"${PLATFORM}\"].sha256" | tr 'A-F' 'a-f')"

[[ -n "$PY_URL" && "$PY_URL" != null ]] || die "python platform not in lock: $PLATFORM"
[[ -n "$LC_URL" && "$LC_URL" != null ]] || die "libclang_ng platform not in lock: $PLATFORM"

LOCK_FP="$(sha256_text "${PLATFORM}|${PY_SHA}|${LC_SHA}" | cut -c1-16)"
ENV_KEY="${PLATFORM}-${LOCK_FP}"

CACHE_ROOT="$(default_cache_root)"
DOWNLOAD_DIR="${CACHE_ROOT}/downloads"
ENV_DIR="${CACHE_ROOT}/envs/${ENV_KEY}"
PYTHON_DIR="${ENV_DIR}/python"
STAMP_PATH="${ENV_DIR}/READY.json"
TMP_ROOT="${CACHE_ROOT}/tmp"

echo "cache: ${CACHE_ROOT}"
echo "env:   ${ENV_KEY}"

stamp_ok=0
if [[ "$FORCE" -eq 0 && -f "$STAMP_PATH" ]]; then
  if command -v jq >/dev/null 2>&1; then
    ST_PLAT="$(jq -r .platform "$STAMP_PATH")"
    ST_KEY="$(jq -r .env_key "$STAMP_PATH")"
    ST_PY="$(jq -r .python_version "$STAMP_PATH")"
    ST_PYSHA="$(jq -r .python_sha256 "$STAMP_PATH")"
    ST_LC="$(jq -r .libclang_ng "$STAMP_PATH")"
    ST_LCSHA="$(jq -r .libclang_ng_sha256 "$STAMP_PATH")"
    ST_EXE="$(jq -r .python_exe "$STAMP_PATH")"
  elif command -v python3 >/dev/null 2>&1; then
    eval "$(python3 - "$STAMP_PATH" <<'PY'
import json,sys
s=json.load(open(sys.argv[1],encoding="utf-8"))
def q(x): return "'" + str(x).replace("'", "'\"'\"'") + "'"
print(f'ST_PLAT={q(s.get("platform",""))}')
print(f'ST_KEY={q(s.get("env_key",""))}')
print(f'ST_PY={q(s.get("python_version",""))}')
print(f'ST_PYSHA={q(s.get("python_sha256",""))}')
print(f'ST_LC={q(s.get("libclang_ng",""))}')
print(f'ST_LCSHA={q(s.get("libclang_ng_sha256",""))}')
print(f'ST_EXE={q(s.get("python_exe",""))}')
PY
)"
  else
    ST_PLAT=""
  fi
  if [[ "${ST_PLAT:-}" == "$PLATFORM" && "${ST_KEY:-}" == "$ENV_KEY" \
     && "${ST_PY:-}" == "$PY_VER" && "${ST_PYSHA:-}" == "$PY_SHA" \
     && "${ST_LC:-}" == "$LC_VER" && "${ST_LCSHA:-}" == "$LC_SHA" \
     && -n "${ST_EXE:-}" && -e "${ST_EXE:-}" ]]; then
    echo "toolchain ready: $ST_EXE"
    PY_EXE_JSON="${ST_EXE//\\/\\\\}"
    CACHE_JSON="${CACHE_ROOT//\\/\\\\}"
    cat > "${CACHE_ROOT}/LAST_ENV.json" <<EOF
{
  "env_key": "${ENV_KEY}",
  "env_dir": "${ENV_DIR//\\/\\\\}",
  "stamp_path": "${STAMP_PATH//\\/\\\\}",
  "python_exe": "${PY_EXE_JSON}",
  "cache_root": "${CACHE_JSON}",
  "platform": "${PLATFORM}"
}
EOF
    echo "$ST_EXE"
    exit 0
  fi
  echo "stamp mismatch, rebuilding env"
fi

mkdir -p "$DOWNLOAD_DIR" "$ENV_DIR" "$TMP_ROOT"

PY_ARCHIVE="${DOWNLOAD_DIR}/${PY_SHA}.tar.gz"
WHEEL_PATH="${DOWNLOAD_DIR}/${LC_SHA}.whl"

echo "python ${PY_VER}"
download_verified "$PY_URL" "$PY_SHA" "$PY_ARCHIVE"

echo "libclang-ng ${LC_VER}"
download_verified "$LC_URL" "$LC_SHA" "$WHEEL_PATH"

rm -rf "$PYTHON_DIR"
mkdir -p "$PYTHON_DIR"
TMP_PY="${TMP_ROOT}/python_extract_$$"
rm -rf "$TMP_PY"
mkdir -p "$TMP_PY"
echo "extracting python..."
tar -xzf "$PY_ARCHIVE" -C "$TMP_PY"
if [[ -d "$TMP_PY/python" ]]; then
  shopt -s dotglob
  mv "$TMP_PY"/python/* "$PYTHON_DIR"/
  shopt -u dotglob
else
  shopt -s dotglob
  mv "$TMP_PY"/* "$PYTHON_DIR"/
  shopt -u dotglob
fi
rm -rf "$TMP_PY"

PY_EXE="$(find_python_exe "$PYTHON_DIR")"
echo "python exe: $PY_EXE"

SITE="$("$PY_EXE" - <<'PY'
import os, site, sys
cands = []
if hasattr(site, "getsitepackages"):
    cands.extend(site.getsitepackages() or [])
cands += [
    os.path.join(sys.prefix, "Lib", "site-packages"),
    os.path.join(sys.prefix, "lib", "site-packages"),
    os.path.join(sys.prefix, "lib", "python3.13", "site-packages"),
]
for p in cands:
    if p and p.replace("\\", "/").rstrip("/").endswith("site-packages"):
        print(p)
        break
else:
    print(os.path.join(sys.prefix, "Lib", "site-packages"))
PY
)"
echo "site-packages: $SITE"
mkdir -p "$SITE"
rm -rf "${SITE}/clang"
rm -rf "${SITE}"/libclang_ng-*.dist-info
TMP_WHEEL="${TMP_ROOT}/wheel_extract_$$"
rm -rf "$TMP_WHEEL"
mkdir -p "$TMP_WHEEL"
echo "extracting wheel..."
if command -v unzip >/dev/null 2>&1; then
  unzip -q "$WHEEL_PATH" -d "$TMP_WHEEL"
else
  tar -xf "$WHEEL_PATH" -C "$TMP_WHEEL"
fi
shopt -s dotglob
mv "$TMP_WHEEL"/* "$SITE"/
shopt -u dotglob
rm -rf "$TMP_WHEEL"

echo "smoke: import clang.cindex"
"$PY_EXE" -c "from clang.cindex import Index, Config; print('clang OK', Config.library_path or Config.library_file or 'bundled')"

# Escape backslashes for JSON on Windows-ish paths if any
PY_EXE_JSON="${PY_EXE//\\/\\\\}"
CACHE_JSON="${CACHE_ROOT//\\/\\\\}"

cat > "$STAMP_PATH" <<EOF
{
  "platform": "${PLATFORM}",
  "env_key": "${ENV_KEY}",
  "python_version": "${PY_VER}",
  "python_sha256": "${PY_SHA}",
  "libclang_ng": "${LC_VER}",
  "libclang_ng_sha256": "${LC_SHA}",
  "python_exe": "${PY_EXE_JSON}",
  "cache_root": "${CACHE_JSON}"
}
EOF
echo "wrote $STAMP_PATH"
cat > "${CACHE_ROOT}/LAST_ENV.json" <<EOF
{
  "env_key": "${ENV_KEY}",
  "env_dir": "${ENV_DIR//\\/\\\\}",
  "stamp_path": "${STAMP_PATH//\\/\\\\}",
  "python_exe": "${PY_EXE_JSON}",
  "cache_root": "${CACHE_JSON}",
  "platform": "${PLATFORM}"
}
EOF
echo "bootstrap done."
echo "$PY_EXE"
