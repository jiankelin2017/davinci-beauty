<#
  P2 离线台架编译脚本（纯 ASCII，避免代码页问题）

  只编对拍所需的四项：gpu_smooth / beauty_smooth / beauty_pool / beauty_log。
  OFX 宿主层不参与，所以不需要 onnxruntime。

  用法：powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
  产物：gpu_smooth_test.exe
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$bench  = $PSScriptRoot
# The source folder name contains non-ASCII characters; this script stays pure
# ASCII, so locate it by pattern instead of hardcoding the name.
$parent = Split-Path $bench -Parent
$srcDir = (Get-ChildItem $parent -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName 'src\gpu_smooth.cpp') } |
    Select-Object -First 1).FullName
if (-not $srcDir) {
    throw "source tree (src\gpu_smooth.cpp) not found next to _p2bench"
}

function Find-VisualStudio {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($path) { return $path.Trim() }
    }
    foreach ($p in @(
            'C:\Program Files\Microsoft Visual Studio\18\Community',
            'C:\Program Files\Microsoft Visual Studio\2022\Community',
            'C:\Program Files\Microsoft Visual Studio\2022\Professional',
            'C:\Program Files\Microsoft Visual Studio\2022\Enterprise')) {
        if (Test-Path $p) { return $p }
    }
    throw 'Visual Studio with the C++ toolset was not found.'
}

$vs = Find-VisualStudio
$msvcRoot = Join-Path $vs 'VC\Tools\MSVC'
$msvc = Get-ChildItem $msvcRoot -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $msvc) { throw "No MSVC toolset under $msvcRoot" }
$vctools = Join-Path $msvcRoot $msvc.Name
$cl = Join-Path $vctools 'bin\Hostx64\x64\cl.exe'
if (-not (Test-Path $cl)) { throw "cl.exe not found: $cl" }

$kits = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVer = (Get-ChildItem (Join-Path $kits 'Include') -Directory |
    Where-Object { $_.Name -match '^10\.' } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1).Name
if (-not $sdkVer) { throw 'Windows SDK not found' }

$env:INCLUDE = @(
    (Join-Path $vctools 'include'),
    (Join-Path $kits "Include\$sdkVer\ucrt"),
    (Join-Path $kits "Include\$sdkVer\shared"),
    (Join-Path $kits "Include\$sdkVer\um")
) -join ';'
$env:LIB = @(
    (Join-Path $vctools 'lib\x64'),
    (Join-Path $kits "Lib\$sdkVer\ucrt\x64"),
    (Join-Path $kits "Lib\$sdkVer\um\x64")
) -join ';'
$env:PATH = (Join-Path $vctools 'bin\Hostx64\x64') + ';' + $env:PATH

$sources = @(
    (Join-Path $srcDir 'src\gpu_smooth.cpp'),
    (Join-Path $srcDir 'src\beauty_smooth.cpp'),
    (Join-Path $srcDir 'src\beauty_pool.cpp'),
    (Join-Path $srcDir 'src\beauty_log.cpp'),
    (Join-Path $bench 'gpu_smooth_test.cpp')
)
foreach ($s in $sources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

$out = Join-Path $bench 'gpu_smooth_test.exe'
$args = @(
    '/nologo', '/O2', '/EHsc', '/MD', '/W3', '/utf-8', '/arch:AVX2', '/std:c++17',
    "/Fe$out", "/Fo$bench\",
    '/D_CRT_SECURE_NO_WARNINGS',
    '/I', (Join-Path $srcDir 'src')
) + $sources + @(
    '/link', 'd3d11.lib', 'dxgi.lib', 'd3dcompiler.lib'
)

Write-Host "[BENCH] MSVC $($msvc.Name) + SDK $sdkVer"
& $cl @args
if ($LASTEXITCODE -ne 0) { throw "[FAILED] cl.exe exited $LASTEXITCODE" }
Write-Host "[OK] $out"
