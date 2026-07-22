#Requires -Version 7.0
<#
.SYNOPSIS
  Codegen entry (Windows). Bootstraps pinned toolchain, then runs a Python script with it.
#>
param(
  [string]$Script = 'scripts/smoke_toolchain.py',
  [switch]$Force,
  [Parameter(ValueFromRemainingArguments = $true)]
  [string[]]$ScriptArgs
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$CodegenRoot = $PSScriptRoot
Push-Location $CodegenRoot
try {
  $bootArgs = @{}
  if ($Force) { $bootArgs.Force = $true }
  $null = & (Join-Path $CodegenRoot 'bootstrap.ps1') @bootArgs

  # Prefer LAST_ENV.json written by bootstrap (stable across PS output quirks).
  $cacheRoot = if ($env:DCB_CODEGEN_CACHE -and $env:DCB_CODEGEN_CACHE.Trim()) {
    $env:DCB_CODEGEN_CACHE.Trim()
  } elseif ($env:LOCALAPPDATA) {
    Join-Path $env:LOCALAPPDATA 'dart_cpp_bridge\toolchain'
  } else {
    Join-Path $HOME 'AppData\Local\dart_cpp_bridge\toolchain'
  }
  $pointerPath = Join-Path $cacheRoot 'LAST_ENV.json'
  if (-not (Test-Path -LiteralPath $pointerPath)) {
    throw "bootstrap did not write LAST_ENV.json at $pointerPath"
  }
  $pointer = Get-Content -LiteralPath $pointerPath -Raw -Encoding utf8 | ConvertFrom-Json
  $py = [string]$pointer.python_exe
  if (-not $py -or -not (Test-Path -LiteralPath $py)) {
    throw "pinned python missing: $py"
  }

  $scriptPath = if ([System.IO.Path]::IsPathRooted($Script)) { $Script } else { Join-Path $CodegenRoot $Script }
  if (-not (Test-Path -LiteralPath $scriptPath)) {
    throw "script not found: $scriptPath"
  }

  Write-Host ">> $py $scriptPath $($ScriptArgs -join ' ')"
  & $py $scriptPath @ScriptArgs
  exit $LASTEXITCODE
}
finally {
  Pop-Location
}
