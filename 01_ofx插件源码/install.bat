@echo off
rem ---------------------------------------------------------------
rem NamaBeauty OFX installer
rem Builds the OFX bundle and installs it to %COMMONPROGRAMFILES%\OFX\Plugins
rem ASCII only: cmd.exe decodes .bat as the system ANSI codepage.
rem
rem IMPORTANT: never rmdir the target directory.
rem   Resolve keeps CNamaSDK.dll / fuai.dll / NamaBeauty.ofx mapped while it
rem   runs, so a "delete then copy" sequence deletes everything it can (the
rem   model/ and graphics/ bundles) and then fails to copy the locked files
rem   back -- leaving a half-empty install that silently falls back to
rem   passthrough. Files are replaced one by one instead; locked files are
rem   renamed aside (Windows allows renaming a mapped file) and re-copied.
rem ---------------------------------------------------------------
setlocal enabledelayedexpansion
cd /d "%~dp0"

set ENGINE=C:\ProgramData\NamaBeauty\engine
set BUNDLE=%~dp0bundle\NamaBeauty.ofx.bundle
set WIN64=%BUNDLE%\Contents\Win64

if not exist "%~dp0out\NamaBeauty.ofx" (
    echo [ERROR] out\NamaBeauty.ofx not found. Run build.bat first.
    exit /b 1
)
rem Engine resources normally live at C:\ProgramData\NamaBeauty\engine. On a machine
rem where the engine only exists inside an already-installed bundle, fall back to
rem that copy: the installer then re-stages the same engine files instead of
rem refusing to run at all.
if not exist "%ENGINE%\CNamaSDK.dll" (
    set "INSTALLED=%COMMONPROGRAMFILES%\OFX\Plugins\NamaBeauty.ofx.bundle\Contents\Win64"
    if exist "!INSTALLED!\CNamaSDK.dll" (
        echo [INFO] engine not at %ENGINE% -- using installed bundle copy:
        echo        !INSTALLED!
        set "ENGINE=!INSTALLED!"
    ) else (
        echo [ERROR] engine not found at %ENGINE%
        echo         and no installed bundle at !INSTALLED!
        exit /b 1
    )
)

echo [1/4] Building bundle layout ...
if exist "%BUNDLE%" rmdir /s /q "%BUNDLE%"
mkdir "%WIN64%" 2>nul
mkdir "%WIN64%\model" 2>nul
mkdir "%WIN64%\graphics" 2>nul

copy /y "%~dp0out\NamaBeauty.ofx" "%WIN64%\" >nul
copy /y "%ENGINE%\CNamaSDK.dll" "%WIN64%\" >nul
copy /y "%ENGINE%\fuai.dll" "%WIN64%\" >nul
copy /y "%ENGINE%\w32-pthreads.dll" "%WIN64%\" >nul
copy /y "%ENGINE%\api-ms-win-downlevel-shlwapi-l1-1-0.dll" "%WIN64%\" >nul
copy /y "%ENGINE%\auth_package.bin" "%WIN64%\" >nul
copy /y "%ENGINE%\model\*.bundle" "%WIN64%\model\" >nul
copy /y "%ENGINE%\graphics\*.bundle" "%WIN64%\graphics\" >nul

echo [2/4] Bundle contents:
dir /s /b "%WIN64%" | findstr /v "^$"

echo [3/4] Installing to OFX plugins dir ...
rem Check the SOURCE variable, not "%OFXDIR%": if %COMMONPROGRAMFILES% were
rem undefined, OFXDIR would expand to "\OFX\Plugins" (non-empty) and the
rem emptiness test below would silently pass.
if "%COMMONPROGRAMFILES%"=="" (
    echo [ERROR] %%COMMONPROGRAMFILES%% is not set.
    exit /b 1
)
set OFXDIR=%COMMONPROGRAMFILES%\OFX\Plugins
set TARGET=%OFXDIR%\NamaBeauty.ofx.bundle\Contents\Win64
if not exist "%TARGET%" mkdir "%TARGET%" 2>nul
if not exist "%TARGET%\model" mkdir "%TARGET%\model" 2>nul
if not exist "%TARGET%\graphics" mkdir "%TARGET%\graphics" 2>nul

rem Install order matters: refresh the data/model files first and replace the
rem host binary (NamaBeauty.ofx) and the engine DLLs LAST. Resolve maps those
rem into the process while running, so doing them last means a failed run never
rem leaves a half-new bundle that silently falls back to passthrough.
set FAILED=0
call :inst "%WIN64%\model\ai_face_processor_pc.bundle"           "%TARGET%\model\ai_face_processor_pc.bundle"
call :inst "%WIN64%\model\ai_human_processor_pc.bundle"          "%TARGET%\model\ai_human_processor_pc.bundle"
call :inst "%WIN64%\graphics\face_beautification.bundle"         "%TARGET%\graphics\face_beautification.bundle"
call :inst "%WIN64%\graphics\face_makeup.bundle"                 "%TARGET%\graphics\face_makeup.bundle"
call :inst "%WIN64%\graphics\body_slim.bundle"                   "%TARGET%\graphics\body_slim.bundle"
call :inst "%WIN64%\graphics\background_blur.bundle"             "%TARGET%\graphics\background_blur.bundle"
call :inst "%WIN64%\auth_package.bin"                            "%TARGET%\auth_package.bin"
call :inst "%WIN64%\w32-pthreads.dll"                            "%TARGET%\w32-pthreads.dll"
call :inst "%WIN64%\api-ms-win-downlevel-shlwapi-l1-1-0.dll"     "%TARGET%\api-ms-win-downlevel-shlwapi-l1-1-0.dll"
call :inst "%WIN64%\CNamaSDK.dll"                                "%TARGET%\CNamaSDK.dll"
call :inst "%WIN64%\fuai.dll"                                    "%TARGET%\fuai.dll"
call :inst "%WIN64%\NamaBeauty.ofx"                              "%TARGET%\NamaBeauty.ofx"

echo [4/4] Verifying ...
set MISSING=0
for %%F in (
    "%TARGET%\NamaBeauty.ofx"
    "%TARGET%\CNamaSDK.dll"
    "%TARGET%\fuai.dll"
    "%TARGET%\auth_package.bin"
    "%TARGET%\model\ai_face_processor_pc.bundle"
    "%TARGET%\graphics\face_beautification.bundle"
) do (
    if not exist %%F (
        echo   [MISSING] %%~F
        set MISSING=1
    )
)
if "%MISSING%"=="1" (
    echo [ERROR] verification failed - some files are missing.
    echo         Close DaVinci Resolve and run install.bat again.
    exit /b 1
)
if "%FAILED%"=="1" (
    echo [WARN] some files could not be refreshed - close Resolve and re-run.
) else (
    echo [OK] installed: %OFXDIR%\NamaBeauty.ofx.bundle
)

rem Match the Python / NSIS installers: drop Resolve's OFX scan cache so the new
rem plugin is picked up on the next launch (no manual cache clear needed).
if not "%APPDATA%"=="" (
    del /f /q "%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml" >nul 2>&1
)
echo.
echo Restart DaVinci Resolve, then look in
rem ASCII only (cmd.exe decodes .bat as the system codepage): describe the Chinese
rem effect label in words instead of embedding the UTF-8 characters.
echo   Effects Library ^> OpenFX ^> NamaBeauty ^> the NamaBeauty effect
echo   (its label is "NamaBeauty" followed by two Chinese characters)
exit /b 0

rem ---------------------------------------------------------------
rem :inst <src> <dst>
rem   copy src over dst; if dst is locked by a running host, rename it
rem   aside and copy again (renaming a mapped file is allowed).
rem ---------------------------------------------------------------
:inst
set "SRC=%~1"
set "DST=%~2"
copy /y "%SRC%" "%DST%" >nul 2>&1
if not errorlevel 1 (
    echo   ok   %~nx2
    exit /b 0
)
set "ASIDE=%DST%.locked-%RANDOM%"
move /y "%DST%" "%ASIDE%" >nul 2>&1
copy /y "%SRC%" "%DST%" >nul 2>&1
if not errorlevel 1 (
    echo   ok   %~nx2 ^(old file kept as %~nx2.locked-*^)
    exit /b 0
)
echo   FAIL %~nx2  ^(locked and could not be replaced - close Resolve^)
set FAILED=1
exit /b 0
