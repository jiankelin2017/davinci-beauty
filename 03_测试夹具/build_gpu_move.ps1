# build_gpu_move.ps1 -- build gpu_move_bench.exe (GPU motion breakdown bench)
#
# ASCII-only on purpose: Windows PowerShell 5.1 reads a BOM-less .ps1 as ANSI,
# so non-ASCII literals get mangled and can swallow the following lines.
#
# vcvars64.bat is blocked in the sandbox (it shells out to reg.exe), so the
# INCLUDE / LIB / PATH are assembled by hand -- same as build.ps1 and
# build_roi.ps1. Compiler flags mirror the production build.

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = Split-Path $here -Parent
$srcRoot = Get-ChildItem $repo -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName 'src\beauty_smooth.cpp') } |
    Select-Object -First 1
if (-not $srcRoot) { throw "source tree not found under $repo" }
$src = Join-Path $srcRoot.FullName 'src'

function Find-VisualStudio {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($path) { return $path.Trim() }
    }
    foreach ($p in @(
            'C:\Program Files\Microsoft Visual Studio\18\Community',
            'C:\Program Files\Microsoft Visual Studio\2022\Community')) {
        if (Test-Path $p) { return $p }
    }
    throw 'Visual Studio with the C++ toolset was not found.'
}

$vs = Find-VisualStudio
$msvcRoot = Join-Path $vs 'VC\Tools\MSVC'
$msvc = Get-ChildItem $msvcRoot -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$vctools = Join-Path $msvcRoot $msvc.Name
$cl = Join-Path $vctools 'bin\Hostx64\x64\cl.exe'

$kits = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVer = Get-ChildItem (Join-Path $kits 'Include') -Directory |
    Where-Object { $_.Name -match '^10\.' } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$sdkVer = $sdkVer.Name

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

Push-Location $here
try {
    & $cl /nologo /O2 /EHsc /MD /W3 /utf-8 /std:c++17 /arch:AVX2 /DNOMINMAX `
        /I"$src" `
        'gpu_move_bench.cpp' `
        (Join-Path $src 'gpu_smooth.cpp') `
        (Join-Path $src 'beauty_smooth.cpp') `
        (Join-Path $src 'beauty_pool.cpp') `
        (Join-Path $src 'beauty_log.cpp') `
        /Fe:gpu_move_bench.exe `
        /link d3d11.lib dxgi.lib d3dcompiler.lib 2>&1 | Select-Object -Last 12
    if ($LASTEXITCODE -ne 0) { throw "cl failed: $LASTEXITCODE" }
    Write-Host "[OK] $here\gpu_move_bench.exe"
}
finally { Pop-Location }
