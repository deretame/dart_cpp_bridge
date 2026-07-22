#Requires -Version 7.0
<#
.SYNOPSIS
  Download + verify pinned Python and libclang-ng into a user-level cache.

  Layout (default root = %LOCALAPPDATA%\dart_cpp_bridge\toolchain):
    downloads/<sha256>.tar.gz|.whl     # blob store, shared across locks
    envs/<platform>-<lockfp16>/        # one unpacked env per lock fingerprint
      python/
      READY.json

  Override root: $env:DCB_CODEGEN_CACHE
#>
param(
  [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$CodegenRoot = $PSScriptRoot
$LockPath = Join-Path $CodegenRoot 'versions.lock'

function Get-PlatformKey {
  $os = if ($IsWindows) { 'windows' } elseif ($IsLinux) { 'linux' } elseif ($IsMacOS) { 'macos' } else {
    throw "Unsupported OS: $([System.Runtime.InteropServices.RuntimeInformation]::OSDescription)"
  }
  $arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
  $archKey = switch ($arch) {
    'X64' { 'x86_64' }
    'Arm64' { 'aarch64' }
    default { throw "Unsupported architecture: $arch" }
  }
  return "$os-$archKey"
}

function Get-DefaultCacheRoot {
  if ($env:DCB_CODEGEN_CACHE -and $env:DCB_CODEGEN_CACHE.Trim()) {
    return $env:DCB_CODEGEN_CACHE.Trim()
  }
  if ($IsWindows) {
    $base = if ($env:LOCALAPPDATA) { $env:LOCALAPPDATA } else { Join-Path $HOME 'AppData\Local' }
    return (Join-Path $base 'dart_cpp_bridge\toolchain')
  }
  if ($IsMacOS) {
    return (Join-Path $HOME 'Library/Caches/dart_cpp_bridge/toolchain')
  }
  $xdg = if ($env:XDG_CACHE_HOME) { $env:XDG_CACHE_HOME } else { Join-Path $HOME '.cache' }
  return (Join-Path $xdg 'dart_cpp_bridge/toolchain')
}

function Get-Sha256File([string]$Path) {
  return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-Sha256Text([string]$Text) {
  $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
  $hash = [System.Security.Cryptography.SHA256]::Create().ComputeHash($bytes)
  return ([System.BitConverter]::ToString($hash) -replace '-', '').ToLowerInvariant()
}

function Ensure-Dir([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path)) {
    New-Item -ItemType Directory -Path $Path | Out-Null
  }
}

function Download-Verified {
  param(
    [string]$Url,
    [string]$Sha256,
    [string]$DestPath
  )
  $expect = $Sha256.ToLowerInvariant()
  Ensure-Dir (Split-Path -Parent $DestPath)
  if (Test-Path -LiteralPath $DestPath) {
    $got = Get-Sha256File $DestPath
    if ($got -eq $expect) {
      Write-Host "  cache hit: $(Split-Path -Leaf $DestPath)"
      return
    }
    Write-Host "  hash mismatch, re-download: $(Split-Path -Leaf $DestPath)"
    Remove-Item -LiteralPath $DestPath -Force
  }
  Write-Host "  downloading: $Url"
  $tmp = "$DestPath.part"
  if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force }
  Invoke-WebRequest -Uri $Url -OutFile $tmp -UseBasicParsing
  $got = Get-Sha256File $tmp
  if ($got -ne $expect) {
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    throw "SHA256 mismatch for $(Split-Path -Leaf $DestPath)`n  expected: $expect`n  got:      $got"
  }
  Move-Item -LiteralPath $tmp -Destination $DestPath -Force
  Write-Host "  verified: $(Split-Path -Leaf $DestPath)"
}

function Find-PythonExe([string]$Root) {
  $candidates = @(
    (Join-Path $Root 'python.exe'),
    (Join-Path $Root 'python\python.exe'),
    (Join-Path $Root 'bin\python3'),
    (Join-Path $Root 'python\bin\python3'),
    (Join-Path $Root 'bin\python'),
    (Join-Path $Root 'python\bin\python')
  )
  foreach ($c in $candidates) {
    if (Test-Path -LiteralPath $c) { return (Resolve-Path -LiteralPath $c).Path }
  }
  throw "python executable not found under $Root"
}

function Get-SitePackages([string]$PyExe) {
  $out = & $PyExe -c @"
import os, site, sys
cands = []
if hasattr(site, 'getsitepackages'):
    cands.extend(site.getsitepackages() or [])
cands.append(os.path.join(sys.prefix, 'Lib', 'site-packages'))
cands.append(os.path.join(sys.prefix, 'lib', 'site-packages'))
cands.append(os.path.join(sys.prefix, 'lib', 'python3.13', 'site-packages'))
for p in cands:
    if p and p.replace('\\\\', '/').rstrip('/').endswith('site-packages'):
        print(p)
        break
else:
    print(os.path.join(sys.prefix, 'Lib', 'site-packages'))
"@ 2>&1
  $lines = @($out | ForEach-Object { "$_".Trim() } | Where-Object { $_ })
  $site = $lines[0]
  Ensure-Dir $site
  return $site
}

function Install-Wheel {
  param(
    [string]$WheelPath,
    [string]$SitePackages,
    [string]$TmpRoot
  )
  Ensure-Dir $SitePackages
  $clangDir = Join-Path $SitePackages 'clang'
  if (Test-Path -LiteralPath $clangDir) {
    Remove-Item -LiteralPath $clangDir -Recurse -Force
  }
  Get-ChildItem -LiteralPath $SitePackages -Directory -Filter 'libclang_ng-*.dist-info' -ErrorAction SilentlyContinue |
    Remove-Item -Recurse -Force

  Write-Host "  extracting wheel -> $SitePackages"
  $tmpExtract = Join-Path $TmpRoot ("wheel_extract_" + [Guid]::NewGuid().ToString('N'))
  Ensure-Dir $tmpExtract
  try {
    tar -xf $WheelPath -C $tmpExtract
    if ($LASTEXITCODE -ne 0) { throw "tar extract wheel failed: $LASTEXITCODE" }
    Get-ChildItem -LiteralPath $tmpExtract | ForEach-Object {
      $dest = Join-Path $SitePackages $_.Name
      if (Test-Path -LiteralPath $dest) {
        Remove-Item -LiteralPath $dest -Recurse -Force
      }
      Move-Item -LiteralPath $_.FullName -Destination $dest -Force
    }
  }
  finally {
    if (Test-Path -LiteralPath $tmpExtract) {
      Remove-Item -LiteralPath $tmpExtract -Recurse -Force -ErrorAction SilentlyContinue
    }
  }
}

function Test-StampMatch($stamp, $expected) {
  return $stamp.platform -eq $expected.platform -and
    $stamp.env_key -eq $expected.env_key -and
    $stamp.python_version -eq $expected.python_version -and
    $stamp.python_sha256 -eq $expected.python_sha256 -and
    $stamp.libclang_ng -eq $expected.libclang_ng -and
    $stamp.libclang_ng_sha256 -eq $expected.libclang_ng_sha256 -and
    $stamp.python_exe -and (Test-Path -LiteralPath $stamp.python_exe)
}

# --- main ---
if (-not (Test-Path -LiteralPath $LockPath)) {
  throw "missing versions.lock: $LockPath"
}

$lock = Get-Content -LiteralPath $LockPath -Raw -Encoding utf8 | ConvertFrom-Json
$platform = Get-PlatformKey
Write-Host "platform: $platform"

$pySpec = $lock.python.platforms.$platform
$lcSpec = $lock.libclang_ng.platforms.$platform
if (-not $pySpec) { throw "python platform not in versions.lock: $platform" }
if (-not $lcSpec) { throw "libclang_ng platform not in versions.lock: $platform" }

$pySha = $pySpec.sha256.ToLowerInvariant()
$lcSha = $lcSpec.sha256.ToLowerInvariant()
$lockFp = (Get-Sha256Text "$platform|$pySha|$lcSha").Substring(0, 16)
$envKey = "$platform-$lockFp"

$cacheRoot = Get-DefaultCacheRoot
$downloadDir = Join-Path $cacheRoot 'downloads'
$envDir = Join-Path $cacheRoot (Join-Path 'envs' $envKey)
$pythonDir = Join-Path $envDir 'python'
$stampPath = Join-Path $envDir 'READY.json'
$tmpRoot = Join-Path $cacheRoot 'tmp'

Write-Host "cache: $cacheRoot"
Write-Host "env:   $envKey"

$expectedStamp = [ordered]@{
  platform           = $platform
  env_key            = $envKey
  python_version     = [string]$lock.python.version
  python_sha256      = $pySha
  libclang_ng        = [string]$lock.libclang_ng.version
  libclang_ng_sha256 = $lcSha
  python_exe         = $null
  cache_root         = $cacheRoot
}

if (-not $Force -and (Test-Path -LiteralPath $stampPath)) {
  $stamp = Get-Content -LiteralPath $stampPath -Raw -Encoding utf8 | ConvertFrom-Json
  if (Test-StampMatch $stamp $expectedStamp) {
    Write-Host "toolchain ready: $($stamp.python_exe)"
    $pointer = [ordered]@{
      env_key     = $envKey
      env_dir     = $envDir
      stamp_path  = $stampPath
      python_exe  = [string]$stamp.python_exe
      cache_root  = $cacheRoot
      platform    = $platform
    }
    $pointerPath = Join-Path $cacheRoot 'LAST_ENV.json'
    $pointer | ConvertTo-Json | Set-Content -LiteralPath $pointerPath -Encoding utf8
    , @{
      PythonExe = [string]$stamp.python_exe
      Platform  = $platform
      EnvKey    = $envKey
      EnvDir    = $envDir
      CacheRoot = $cacheRoot
    }
    return
  }
  Write-Host "stamp mismatch, rebuilding env"
}

Ensure-Dir $downloadDir
Ensure-Dir $envDir
Ensure-Dir $tmpRoot

# Blob names are content-addressed by sha256 (shared across lock envs).
$pyExt = if ($pySpec.url -match '\.tar\.gz') { '.tar.gz' } else { [System.IO.Path]::GetExtension(([Uri]$pySpec.url).AbsolutePath) }
if (-not $pyExt) { $pyExt = '.tar.gz' }
$lcExt = '.whl'
$pyArchive = Join-Path $downloadDir ($pySha + $pyExt)
$wheelPath = Join-Path $downloadDir ($lcSha + $lcExt)

Write-Host "python $($lock.python.version)"
Download-Verified -Url $pySpec.url -Sha256 $pySha -DestPath $pyArchive

Write-Host "libclang-ng $($lock.libclang_ng.version)"
Download-Verified -Url $lcSpec.url -Sha256 $lcSha -DestPath $wheelPath

if (Test-Path -LiteralPath $pythonDir) {
  Remove-Item -LiteralPath $pythonDir -Recurse -Force
}
Ensure-Dir $pythonDir

Write-Host "extracting python..."
$tmpPy = Join-Path $tmpRoot ("python_extract_" + [Guid]::NewGuid().ToString('N'))
Ensure-Dir $tmpPy
try {
  tar -xzf $pyArchive -C $tmpPy
  if ($LASTEXITCODE -ne 0) { throw "tar extract python failed: $LASTEXITCODE" }
  $inner = Join-Path $tmpPy 'python'
  $srcRoot = if (Test-Path -LiteralPath $inner) { $inner } else { $tmpPy }
  Get-ChildItem -LiteralPath $srcRoot | ForEach-Object {
    Move-Item -LiteralPath $_.FullName -Destination (Join-Path $pythonDir $_.Name) -Force
  }
}
finally {
  if (Test-Path -LiteralPath $tmpPy) {
    Remove-Item -LiteralPath $tmpPy -Recurse -Force -ErrorAction SilentlyContinue
  }
}

$pyExe = Find-PythonExe $pythonDir
Write-Host "python exe: $pyExe"

$site = Get-SitePackages $pyExe
Write-Host "site-packages: $site"
Install-Wheel -WheelPath $wheelPath -SitePackages $site -TmpRoot $tmpRoot

Write-Host "smoke: import clang.cindex"
$smokeOut = & $pyExe -c "from clang.cindex import Index, Config; print('clang OK', Config.library_path or Config.library_file or 'bundled')" 2>&1
foreach ($line in @($smokeOut)) { Write-Host $line }
if ($LASTEXITCODE -ne 0) { throw "clang import smoke failed" }

$expectedStamp.python_exe = $pyExe
$expectedStamp | ConvertTo-Json | Set-Content -LiteralPath $stampPath -Encoding utf8
$pointer = [ordered]@{
  env_key     = $envKey
  env_dir     = $envDir
  stamp_path  = $stampPath
  python_exe  = $pyExe
  cache_root  = $cacheRoot
  platform    = $platform
}
$pointerPath = Join-Path $cacheRoot 'LAST_ENV.json'
$pointer | ConvertTo-Json | Set-Content -LiteralPath $pointerPath -Encoding utf8
Write-Host "wrote $stampPath"
Write-Host "bootstrap done."

, @{
  PythonExe = $pyExe
  Platform  = $platform
  EnvKey    = $envKey
  EnvDir    = $envDir
  CacheRoot = $cacheRoot
}
