<#
  DaVinci Beauty OFX build script (MSVC, no CMake).

  Why PowerShell instead of the old .bat: the repository paths contain non-ASCII
  characters, and cmd.exe decodes batch-file bytes with the active OEM code page.
  A GBK-encoded .bat breaks under 65001 and a UTF-8 .bat breaks under 936,
  producing "cannot open include file: onnxruntime_c_api.h". PowerShell receives
  argv as UTF-16, so toolchain paths are unambiguous. This script itself is
  deliberately pure ASCII so it parses correctly under any code page.

  Usage:
    powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
    powershell ... -File build.ps1 -Clean        # remove obj/ first
    powershell ... -File build.ps1 -NoInstall    # compile only

  Output: out\DaVinciBeauty.ofx
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$NoInstall,
    [switch]$Sandbox
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

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

function Find-WindowsSdk {
    $kits = 'C:\Program Files (x86)\Windows Kits\10'
    $include = Join-Path $kits 'Include'
    if (-not (Test-Path $include)) { throw "Windows SDK not found under $kits" }
    $ver = Get-ChildItem $include -Directory |
        Where-Object { $_.Name -match '^10\.' } |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1
    if (-not $ver) { throw "No Windows 10/11 SDK under $include" }
    return @{ Root = $kits; Version = $ver.Name }
}

# Engine assets live in a sibling folder whose numeric prefix has changed over
# time (02_*, 07_*). Match on the asset itself instead of the folder name, and
# prefer a numbered folder when several copies exist.
function Find-EngineDir {
    $parent = Split-Path $root -Parent
    $candidates = Get-ChildItem $parent -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'onnxruntime.dll') }
    if (-not $candidates) { return $null }
    $numbered = $candidates | Where-Object { $_.Name -match '^\d\d_' }
    if ($numbered) { return ($numbered | Select-Object -First 1).FullName }
    return $candidates[0].FullName
}

$vs = Find-VisualStudio
$msvcRoot = Join-Path $vs 'VC\Tools\MSVC'
$msvc = Get-ChildItem $msvcRoot -Directory |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
if (-not $msvc) { throw "No MSVC toolset under $msvcRoot" }
$vctools = Join-Path $msvcRoot $msvc.Name
$cl = Join-Path $vctools 'bin\Hostx64\x64\cl.exe'
if (-not (Test-Path $cl)) { throw "cl.exe not found: $cl" }

$sdk = Find-WindowsSdk
$sdkVer = $sdk.Version
$env:INCLUDE = @(
    (Join-Path $vctools 'include'),
    (Join-Path $sdk.Root "Include\$sdkVer\ucrt"),
    (Join-Path $sdk.Root "Include\$sdkVer\shared"),
    (Join-Path $sdk.Root "Include\$sdkVer\um")
) -join ';'
$env:LIB = @(
    (Join-Path $vctools 'lib\x64'),
    (Join-Path $sdk.Root "Lib\$sdkVer\ucrt\x64"),
    (Join-Path $sdk.Root "Lib\$sdkVer\um\x64")
) -join ';'
$env:PATH = (Join-Path $vctools 'bin\Hostx64\x64') + ';' + $env:PATH

$obj = Join-Path $root 'obj'
$out = Join-Path $root 'out'
if ($Clean -and (Test-Path $obj)) { Remove-Item $obj -Recurse -Force }
foreach ($d in @($obj, $out)) { if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d | Out-Null } }

$engine = Find-EngineDir
if (-not $engine) { throw 'Engine assets folder (07_*/onnxruntime.dll) not found next to the source tree.' }
$ortInclude = Join-Path $engine 'onnxruntime\include'
if (-not (Test-Path (Join-Path $ortInclude 'onnxruntime_c_api.h'))) {
    throw "ONNX Runtime headers not found: $ortInclude"
}

# Sandbox build: swap plugin identifier/label so the sandbox bundle cannot
# shadow the real one (see the -Sandbox switch below).
# Plain if/else on purpose: the `$x = if (...) {...} else {...}` form silently
# yielded an empty string here even though $Sandbox was True.
$sandboxDefine = ''
if ($Sandbox) { $sandboxDefine = '/DNAMA_SANDBOX_BUILD' }
if ($env:NAMA_SANDBOX -eq '1') { $sandboxDefine = '/DNAMA_SANDBOX_BUILD' }

$sources = Get-ChildItem (Join-Path $root 'src') -Filter *.cpp | Sort-Object Name | ForEach-Object { $_.FullName }
if (-not $sources) { throw 'No .cpp sources under src\' }

$clArgs = @(
    '/nologo', '/O2', '/EHsc', '/MD', '/W3', '/utf-8', '/arch:AVX2', '/std:c++17',
    # NOTE: /Brepro on cl does NOT make builds reproducible on this toolchain.
    # See docs/reference/reference-performance.md section 17. The two TimeDateStamp dwords are zeroed
    # after linking instead (see Reset-PeTimestamp below).
    '/Brepro',
    "/Fo$obj\", "/Fe$(Join-Path $out 'DaVinciBeauty.ofx')", '/LD',
    '/DUNICODE', '/D_UNICODE', '/D_CRT_SECURE_NO_WARNINGS',
    # Sandbox build (-Sandbox / NAMA_SANDBOX=1): distinct plugin id + label so the
    # sandbox bundle cannot shadow the real one. Empty on normal builds and
    # filtered out before invoking cl.
    $sandboxDefine,
    '/I', (Join-Path $root 'sdk\include'),
    '/I', $ortInclude
) + $sources + @(
    # /Brepro is NOT what makes builds reproducible here -- on this toolchain it
    # alone still leaves two TimeDateStamp dwords differing between builds (see
    # the note above and Reset-PeTimestamp below). Reproducibility comes from
    # zeroing those dwords after linking. Kept for intent/consistency only.
    #
    # d3d11/dxgi/d3dcompiler: GPU capability probe + bandwidth calibration.
    # advapi32: registry read for CPU base clock (diagnostics only).
    # opengl32/gdi32: legacy GL backend leftovers; no wgl/glCreate calls remain.
    # Kept only to avoid changing existing link behaviour.
    '/link', '/Brepro',
    'd3d11.lib', 'dxgi.lib', 'd3dcompiler.lib', 'advapi32.lib',
    'opengl32.lib', 'gdi32.lib', 'user32.lib',
    "/IMPLIB:$(Join-Path $obj 'DaVinciBeauty.lib')"
)

$clArgs = $clArgs | Where-Object { $_ -ne '' -and $null -ne $_ }
if ($env:NAMA_BUILD_LOG) {
    "[def=$sandboxDefine sw=$Sandbox env=$env:NAMA_SANDBOX] " + ($clArgs -join ' ') |
        Out-File -FilePath $env:NAMA_BUILD_LOG -Encoding utf8 -Append
}
Write-Host "[BUILD] MSVC $($msvc.Name) + SDK $sdkVer"
Write-Host "[BUILD] engine: $engine"
Write-Host "[BUILD] $($sources.Count) sources -> out\DaVinciBeauty.ofx"
# Response file instead of splatting: @clArgs silently dropped elements here
# (a /D define never reached the compiler although it was in the array).
$rsp = Join-Path $obj 'cl.rsp'
Set-Content -Path $rsp -Value ($clArgs -join ' ') -Encoding Default
& $cl "@$rsp"
if ($LASTEXITCODE -ne 0) { throw "[FAILED] cl.exe exited $LASTEXITCODE" }

$dll = Join-Path $out 'DaVinciBeauty.ofx'

# ---- Normalize PE timestamps (see docs/reference/reference-performance.md s17) ----
# /Brepro does not work on this toolchain (neither on cl nor on link): two builds
# from identical sources differ by exactly two dwords -- COFF TimeDateStamp and
# IMAGE_DEBUG_DIRECTORY.TimeDateStamp. Zero both here so builds are reproducible.
# This file must stay PURE ASCII: PowerShell decodes script bytes with the active
# code page, and non-ASCII comments break parsing under the UTF-8/GBK mismatch.
function Reset-PeTimestamp([string]$Path) {
    $b = [IO.File]::ReadAllBytes($Path)
    $lf = [BitConverter]::ToInt32($b, 0x3C)
    if ([Text.Encoding]::ASCII.GetString($b, $lf, 4) -ne "PE`0`0") { return $false }
    $coff = $lf + 4
    [Array]::Clear($b, $coff + 4, 4)
    $nsec  = [BitConverter]::ToUInt16($b, $coff + 2)
    $szOpt = [BitConverter]::ToUInt16($b, $coff + 16)
    $opt   = $lf + 24
    $magic = [BitConverter]::ToUInt16($b, $opt)
    $dd    = $opt + $(if ($magic -eq 0x20B) { 112 } else { 96 })
    $dbgRva  = [BitConverter]::ToUInt32($b, $dd + 6 * 8)
    $dbgSize = [BitConverter]::ToUInt32($b, $dd + 6 * 8 + 4)
    if ($dbgRva -ne 0 -and $dbgSize -ge 28) {
        $sec = $opt + $szOpt
        for ($i = 0; $i -lt $nsec; $i++) {
            $s   = $sec + $i * 40
            $vsz = [BitConverter]::ToUInt32($b, $s + 8)
            $va  = [BitConverter]::ToUInt32($b, $s + 12)
            $rsz = [BitConverter]::ToUInt32($b, $s + 16)
            $raw = [BitConverter]::ToUInt32($b, $s + 20)
            $span = if ($vsz -gt $rsz) { $vsz } else { $rsz }
            if ($dbgRva -ge $va -and $dbgRva -lt ($va + $span)) {
                $fo = $raw + ($dbgRva - $va)
                for ($k = 0; $k -lt [int]($dbgSize / 28); $k++) {
                    [Array]::Clear($b, $fo + $k * 28 + 4, 4)
                }
                break
            }
        }
    }
    [IO.File]::WriteAllBytes($Path, $b)
    return $true
}
if (-not (Reset-PeTimestamp $dll)) { Write-Host '[WARN] PE timestamp normalization failed' }

$size = (Get-Item $dll).Length
Write-Host ("[OK] out\DaVinciBeauty.ofx  {0:N0} bytes  md5={1}" -f $size, (Get-FileHash $dll -Algorithm MD5).Hash.ToLower())

# Stage the engine runtime next to the plugin binary. CoreEngine resolves
# onnxruntime.dll and yunet.onnx from the *plugin's own directory*
# (beauty_core.cpp: selfDir()), not from the NAMA_ENGINE_DIR argument, so a clean
# build without this copy silently degrades to "no face detected" and the
# baseline/mock-host self-tests fail with 0/18 on a fresh checkout.
foreach ($asset in @('onnxruntime.dll', 'yunet.onnx')) {
    $src = Join-Path $engine $asset
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $out $asset) -Force
        Write-Host "[STAGE] out\$asset"
    } else {
        Write-Warning "engine asset missing: $src (self-test will degrade to no-face)"
    }
}

if (-not $NoInstall) {
    Write-Host '[NEXT] run install.bat (or verify.ps1) to stage + install the bundle'
}
