<#
  pool_regress.ps1 - build and run the parallelRows invariant regression.

  Why a separate script: the regression must run in its own process. A deadlock
  inside parallelRows would hang the whole acceptance run, so the test carries
  its own watchdog and this driver treats "no exit within the timeout" as FAIL
  instead of waiting forever.

  This file must stay PURE ASCII: PowerShell decodes script bytes with the
  active code page, and a non-ASCII comment silently eats the next line (the
  root cause of the 2026-11-08 sandbox build failure). The source directory is
  therefore located by content, not by its Chinese name.

  Usage:
    powershell -NoProfile -ExecutionPolicy Bypass -File pool_regress.ps1
    powershell ... -File pool_regress.ps1 -Force      # rebuild unconditionally

  Exit codes: 0 pass / 1 fail / 9 skipped (toolchain or source missing)
#>
[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = Split-Path $here -Parent

# ---- locate the source tree by content (avoid non-ASCII path literals) -----
$src = $null
$cand = Get-ChildItem $repo -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName 'src\beauty_pool.cpp') }
if ($cand) { $src = Join-Path ($cand | Select-Object -First 1).FullName 'src' }
if (-not $src) {
    Write-Host '[POOL] SKIP  src\beauty_pool.cpp not found next to 04_* folder'
    exit 9
}

$objDir = Join-Path $here 'obj'
$exe    = Join-Path $objDir 'pool_regress.exe'
$test   = Join-Path $here 'pool_regress.cpp'
if (-not (Test-Path $objDir)) { New-Item -ItemType Directory -Path $objDir | Out-Null }

# ---- toolchain -------------------------------------------------------------
function Find-VisualStudio {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $p = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($p) { return $p.Trim() }
    }
    foreach ($c in @(
            'C:\Program Files\Microsoft Visual Studio\18\Community',
            'C:\Program Files\Microsoft Visual Studio\2022\Community',
            'C:\Program Files\Microsoft Visual Studio\2022\Professional',
            'C:\Program Files\Microsoft Visual Studio\2022\Enterprise')) {
        if (Test-Path $c) { return $c }
    }
    return $null
}

function Find-WindowsSdk {
    $kits = 'C:\Program Files (x86)\Windows Kits\10'
    $inc = Join-Path $kits 'Include'
    if (-not (Test-Path $inc)) { return $null }
    $ver = Get-ChildItem $inc -Directory | Where-Object { $_.Name -match '^10\.' } |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if (-not $ver) { return $null }
    return @{ Root = $kits; Version = $ver.Name }
}

# Rebuild when the exe is missing, older than any input, or -Force.
$needBuild = $Force -or (-not (Test-Path $exe))
if (-not $needBuild) {
    $t = (Get-Item $exe).LastWriteTime
    foreach ($f in @($test,
                     (Join-Path $src 'beauty_pool.cpp'),
                     (Join-Path $src 'beauty_pool.h'))) {
        if ((Get-Item $f).LastWriteTime -gt $t) { $needBuild = $true }
    }
}

if ($needBuild) {
    $vs = Find-VisualStudio
    if (-not $vs) { Write-Host '[POOL] SKIP  Visual Studio C++ toolset not found'; exit 9 }
    $msvcRoot = Join-Path $vs 'VC\Tools\MSVC'
    $msvc = Get-ChildItem $msvcRoot -Directory | Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1
    if (-not $msvc) { Write-Host '[POOL] SKIP  no MSVC toolset'; exit 9 }
    $vctools = Join-Path $msvcRoot $msvc.Name
    $cl = Join-Path $vctools 'bin\Hostx64\x64\cl.exe'
    if (-not (Test-Path $cl)) { Write-Host '[POOL] SKIP  cl.exe not found'; exit 9 }
    $sdk = Find-WindowsSdk
    if (-not $sdk) { Write-Host '[POOL] SKIP  Windows SDK not found'; exit 9 }

    $env:INCLUDE = @(
        (Join-Path $vctools 'include'),
        (Join-Path $sdk.Root "Include\$($sdk.Version)\ucrt"),
        (Join-Path $sdk.Root "Include\$($sdk.Version)\shared"),
        (Join-Path $sdk.Root "Include\$($sdk.Version)\um")
    ) -join ';'
    $env:LIB = @(
        (Join-Path $vctools 'lib\x64'),
        (Join-Path $sdk.Root "Lib\$($sdk.Version)\ucrt\x64"),
        (Join-Path $sdk.Root "Lib\$($sdk.Version)\um\x64")
    ) -join ';'

    $clArgs = @(
        '/nologo', '/O2', '/EHsc', '/MD', '/W3', '/std:c++17', '/utf-8',
        "/Fo$objDir\", "/Fe$exe",
        '/I', $src,
        (Join-Path $src 'beauty_pool.cpp'),
        $test
    )
    Write-Host '[POOL] building pool_regress.exe'
    & $cl @clArgs | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Host "[POOL] RESULT FAIL  build exit=$LASTEXITCODE"; exit 1 }
}

# ---- run -------------------------------------------------------------------
# Direct invocation on purpose: Start-Process chokes here on hosts that expose
# both http_proxy and HTTP_PROXY (duplicate dictionary key). The test carries its
# own 20 s watchdog, and acceptance.py wraps this script in a timeout as well.
Write-Host "[POOL] running $exe"
& $exe
$rc = $LASTEXITCODE
if ($null -eq $rc) { $rc = 1 }
Write-Host "[POOL] exit=$rc"
exit $rc
