@echo off
rem =============================================================
rem DaVinci Beauty OFX installer v3.0.0 self-contained open engine
rem No legacy closed-source SDK, uses CoreEngine plus ONNX face model
rem Auto-cleans legacy engine leftovers in the target dir
rem Must run as Administrator to write under C:\Program Files
rem =============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"
echo [install.bat v3] running

rem Locate engine assets dir that holds onnxruntime.dll + yunet.onnx (name-independent)
set "P=%~dp0"
set "P=%P:~0,-1%"
for %%I in ("%P%") do set "PARENT=%%~dpI"
set "ENGINE="
for /d %%D in ("%PARENT%\*") do (
    if exist "%%D\onnxruntime.dll" (
        if exist "%%D\yunet.onnx" set "ENGINE=%%D"
    )
)
if "%ENGINE%"=="" (
    echo [ERROR] Cannot find engine assets folder containing onnxruntime.dll + yunet.onnx.
    echo Make sure the 02_引擎资产 folder still has both files.
    pause
    exit /b 1
)
echo engine dir: %ENGINE%

set "BUNDLE=%~dp0bundle\DaVinciBeauty.ofx.bundle"
set "WIN64=%BUNDLE%\Contents\Win64"

if not exist "%~dp0out\DaVinciBeauty.ofx" (
    echo [ERROR] out\DaVinciBeauty.ofx not found. Run build.bat first.
    pause
    exit /b 1
)
if not exist "%ENGINE%\yunet.onnx" (
    echo [ERROR] Face model not found: %ENGINE%\yunet.onnx
    pause
    exit /b 1
)

echo [1/4] Building bundle layout
if exist "%BUNDLE%" rmdir /s /q "%BUNDLE%"
mkdir "%WIN64%" 2>nul

copy /y "%~dp0out\DaVinciBeauty.ofx" "%WIN64%\" >nul
copy /y "%ENGINE%\onnxruntime.dll" "%WIN64%\" >nul
copy /y "%ENGINE%\yunet.onnx" "%WIN64%\" >nul
rem GPL / MIT / BSD-3 都要求「随副本附带声明」，所以许可文件跟 DLL 一起装。
rem 用 if exist 保护：源码包可能被裁剪成只有 01_ofx插件源码 一层。
if exist "%~dp0..\LICENSE" copy /y "%~dp0..\LICENSE" "%WIN64%\" >nul
if exist "%~dp0..\THIRD-PARTY-NOTICES.md" copy /y "%~dp0..\THIRD-PARTY-NOTICES.md" "%WIN64%\" >nul

echo [2/4] Bundle contents
dir /s /b "%WIN64%" | findstr /v "^$"

echo [3/4] Installing to OFX plugins dir
if "%COMMONPROGRAMFILES%"=="" (
    echo [ERROR] COMMONPROGRAMFILES is not set.
    pause
    exit /b 1
)
set "OFXDIR=%COMMONPROGRAMFILES%\OFX\Plugins"
set "TARGET=%OFXDIR%\DaVinciBeauty.ofx.bundle\Contents\Win64"
if not exist "%TARGET%" mkdir "%TARGET%" 2>nul
rem Write-access probe. Elevation is not the criterion: a restricted admin
rem token can still hold the destination ACL, and an elevated shell can still
rem be denied by policy. Creating the directory is the honest test.
if not exist "%TARGET%" (
    echo [ERROR] Cannot create %TARGET%
    echo Right-click install.bat and choose Run as administrator.
    pause
    exit /b 1
)

rem Auto-clean legacy engine leftovers, v3.0.0 dropped the old SDK
echo cleaning legacy engine leftovers
for %%F in (
    "CNamaSDK.dll" "fuai.dll" "ai_face_processor_pc_plain.bundle"
    "auth_package.bin" "api-ms-win-downlevel-shlwapi-l1-1-0.dll" "w32-pthreads.dll"
) do (
    if exist "%TARGET%\%%F" del /f /q "%TARGET%\%%F" >nul 2>&1
)
if exist "%TARGET%\model" rmdir /s /q "%TARGET%\model" >nul 2>&1
if exist "%TARGET%\graphics" rmdir /s /q "%TARGET%\graphics" >nul 2>&1

set "FAILED=0"
call :inst "%WIN64%\yunet.onnx"        "%TARGET%\yunet.onnx"
call :inst "%WIN64%\onnxruntime.dll"   "%TARGET%\onnxruntime.dll"
call :inst "%WIN64%\DaVinciBeauty.ofx" "%TARGET%\DaVinciBeauty.ofx"
if exist "%WIN64%\LICENSE" call :inst "%WIN64%\LICENSE" "%TARGET%\LICENSE"
if exist "%WIN64%\THIRD-PARTY-NOTICES.md" call :inst "%WIN64%\THIRD-PARTY-NOTICES.md" "%TARGET%\THIRD-PARTY-NOTICES.md"

echo [4/4] Verifying
set "MISSING=0"
for %%F in (
    "%TARGET%\DaVinciBeauty.ofx"
    "%TARGET%\onnxruntime.dll"
    "%TARGET%\yunet.onnx"
) do (
    if not exist %%F (
        echo [MISSING] %%~F
        set "MISSING=1"
    )
)
if "%MISSING%"=="1" (
    echo [ERROR] verification failed, some files are missing.
    echo Close DaVinci Resolve and run install.bat again.
    pause
    exit /b 1
)
if "%FAILED%"=="1" (
    echo [WARN] some files could not be refreshed, close Resolve and re-run.
) else (
    echo [OK] installed: %OFXDIR%\DaVinciBeauty.ofx.bundle
)

if not "%APPDATA%"=="" (
    del /f /q "%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml" >nul 2>&1
)
echo.
echo Restart DaVinci Resolve, then look in
echo Effects Library then OpenFX then DaVinci Beauty then the DaVinci Beauty effect
echo log: C:\ProgramData\DaVinciBeauty\logs\ofx.log
echo look for provider=CPU or provider=DirectML
pause
exit /b 0

rem :inst src dst copies src over dst, if dst is locked by a running host
rem rename it aside and copy again since renaming a mapped file is allowed
:inst
set "SRC=%~1"
set "DST=%~2"
copy /y "%SRC%" "%DST%" >nul 2>&1
if not errorlevel 1 (
    echo ok   %~nx2
    exit /b 0
)
set "ASIDE=%DST%.locked-%RANDOM%"
move /y "%DST%" "%ASIDE%" >nul 2>&1
copy /y "%SRC%" "%DST%" >nul 2>&1
if not errorlevel 1 (
    echo ok   %~nx2 old file kept as locked-star
    exit /b 0
)
echo FAIL %~nx2 locked and could not be replaced, close Resolve
set "FAILED=1"
exit /b 0
