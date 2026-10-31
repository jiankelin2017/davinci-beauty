@echo off
setlocal enabledelayedexpansion
title DaVinci Beauty sandbox install verification

rem ============================================================
rem Sandbox install / uninstall verification
rem Builds with APPID=DaVinciBeauty.SandboxTest and
rem bundle=DaVinciBeautySandbox.ofx.bundle, so it never touches the
rem real install at DaVinciBeauty.ofx.bundle. Requires one UAC prompt.
rem ============================================================

rem Only the sandbox installer produced a file named *_sandbox.
rem If sandbox and real builds share the same name/path, the sandbox build
rem would overwrite the real one, then this script would verify the real
rem install and scrub it -- so we only look for the _sandbox package.
set "EXE="
for %%D in ("%~dp0." "%~dp0.." "%~dp0..\.." "%~dp0build") do (
  if not defined EXE for %%F in ("%%~fD\DaVinciBeauty_Setup_v*_sandbox.exe") do (
    if exist "%%~fF" set "EXE=%%~fF"
  )
)
set "SB=C:\Program Files\Common Files\OFX\Plugins\DaVinciBeautySandbox.ofx.bundle"
set "W64=%SB%\Contents\Win64"
set "REGKEY=HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\DaVinciBeauty.SandboxTest"
set "FAILS=0"

echo.
echo ============================================================
echo   Sandbox verification (isolated from real install)
echo ============================================================
if defined EXE (echo   installer: %EXE%) else (echo   installer: ^(not found^))
echo   target  : %SB%
echo.

if not defined EXE (
  echo [XX] sandbox installer not found ^(DaVinciBeauty_Setup_v*_sandbox.exe^)
  echo.
  echo     This script only verifies the sandbox build.
  echo     To build it: installer\build_nsis.py --sandbox
  echo     which writes build\DaVinciBeauty_Setup_v^<version^>_sandbox.exe
  echo.
  echo     For the real install: double-click build\DaVinciBeauty_Setup_v^<version^>.exe
  echo     -- no script needed.
  set "FAILS=-1"
  goto done
)

rem ---------- step 1: install ----------
echo ------------------------------------------------------------
echo [1/4] default install ...  UAC prompt will appear
echo ------------------------------------------------------------
"%EXE%" /S
if errorlevel 1 (
  echo [XX] install failed with %errorlevel%
  set /a FAILS+=1
)
timeout /t 3 /nobreak >nul

rem ---------- step 2: verify installed files ----------
echo.
echo ------------------------------------------------------------
echo [2/4] verify installed files
echo ------------------------------------------------------------
call :checkfile "plugin binary"            "%W64%\DaVinciBeauty.ofx"
call :checkfile "ONNX Runtime"             "%W64%\onnxruntime.dll"
call :checkfile "face detection model"     "%W64%\yunet.onnx"
call :checkfile "uninstaller"              "%SB%\Uninstall_DaVinciBeauty.exe"
call :checkreg  "uninstall registry key"

echo.
echo ---- make sure Resolve scan dir has no stray python folder ----
if exist "%W64%\python" (
  echo [XX] Contents\Win64 has a python folder (breaks OFX scan^)
  set /a FAILS+=1
) else (
  echo [OK] no python folder
)

echo.
echo step 1/2 done. Press any key to verify uninstall.
pause

rem ---------- step 3: uninstall ----------
echo.
echo ------------------------------------------------------------
echo [3/4] default uninstall ...  UAC prompt will appear
echo ------------------------------------------------------------
if exist "%SB%\Uninstall_DaVinciBeauty.exe" (
  "%SB%\Uninstall_DaVinciBeauty.exe" /S
  if errorlevel 1 (
    echo [XX] uninstall failed with %errorlevel%
    set /a FAILS+=1
  )
  timeout /t 3 /nobreak >nul
) else (
  echo [XX] uninstaller not found
  set /a FAILS+=1
)

rem ---------- step 4: uninstall results ----------
echo.
echo ------------------------------------------------------------
echo [4/4] uninstall results
echo ------------------------------------------------------------
if exist "%SB%" (
  echo [XX] bundle dir still exists: %SB%
  set /a FAILS+=1
) else (
  echo [OK] bundle dir removed
)
reg query "%REGKEY%" >nul 2>&1
if errorlevel 1 (echo [OK] uninstall registry key removed) else (
  echo [XX] uninstall registry key remains & set /a FAILS+=1
)
reg query "HKLM\SOFTWARE\DaVinciBeauty.SandboxTest" >nul 2>&1
if errorlevel 1 (echo [OK] product registry key removed) else (
  echo [XX] product registry key remains & set /a FAILS+=1
)
if exist "C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle" (
  echo [OK] real install untouched
) else (
  echo [!!] real bundle missing ^(this script should not have touched it^)
)

goto done

rem ---------- helpers ----------
:checkfile
if exist "%~2" (
  echo [OK] %~1
) else (
  echo [XX] %~1  --  missing: %~2
  set /a FAILS+=1
)
goto :eof

:checkreg
reg query "%REGKEY%" >nul 2>&1
if errorlevel 1 (
  echo [XX] %~1  --  registry key not found
  set /a FAILS+=1
) else (
  echo [OK] %~1
)
goto :eof

:done
echo.
echo ============================================================
if "%FAILS%"=="-1" (
  echo   verdict: not executed - missing sandbox installer, no impact
) else (
  if "%FAILS%"=="0" (
    echo   verdict: all passed
  ) else (
    echo   verdict: %FAILS% item^(s^) not passed, see above
  )
)
echo ============================================================
echo.
pause
