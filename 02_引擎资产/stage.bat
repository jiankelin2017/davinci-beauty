@echo off
rem ------------------------------------------------------------
rem Stage engine runtime assets into C:\ProgramData\DaVinciBeauty\engine
rem
rem The self-developed engine (OBS CoreEngine + ONNX YuNet) needs only two
rem runtime files next to the plugin: onnxruntime.dll and yunet.onnx.
rem This copies them from 07_引擎资产 into the shared ProgramData engine dir.
rem (install.bat can also read straight from 07_引擎资产, so this step is
rem  optional.)
rem ------------------------------------------------------------
setlocal
set "SRC=%~dp0"
set "DST=C:\ProgramData\DaVinciBeauty\engine"

echo.
echo Staging engine runtime assets
echo   source: %SRC%
echo   target: %DST%
echo.

if not exist "%SRC%onnxruntime.dll" (
  echo [XX] onnxruntime.dll not found in 07_引擎资产.
  goto done
)
if not exist "%SRC%yunet.onnx" (
  echo [XX] yunet.onnx not found in 07_引擎资产.
  goto done
)

if not exist "%DST%" mkdir "%DST%"
if errorlevel 1 (
  echo [XX] cannot create %DST% (need admin?)
  goto done
)

copy /y "%SRC%onnxruntime.dll" "%DST%\" >nul
copy /y "%SRC%yunet.onnx" "%DST%\" >nul

echo Staged files:
dir /b "%DST%" 2>nul
echo.
echo [OK] Next: close DaVinci Resolve fully, then run
echo      ...\01_ofx插件源码\install.bat

:done
echo.
pause
