@echo off
rem ===========================================================================
rem  Launch DaVinci Resolve with BEAUTY_GPU_PIPE=1 (P1 GPU pipeline check).
rem
rem  Why this file exists: the GPU transfer pipeline is a verification-only
rem  switch (runs an identity compute pass and discards the result, purely to
rem  measure round-trip cost). It is OFF by default. Launching Resolve from its
rem  desktop icon does NOT inherit the variable, so ofx.log has no
rem  "GPU backend ready" / "GPU pipeline" lines. That is not a bug.
rem
rem  Usage:
rem    1. Save your project in Resolve, then quit Resolve completely.
rem    2. Double-click this file.
rem    3. Open the project, drop DaVinci Beauty on a clip with a face, play.
rem    4. Check C:\ProgramData\DaVinciBeauty\logs\ofx.log
rem
rem  Two lessons baked in (both cost a round-trip on 2026-11-06):
rem   * start MUST pass /D <install dir>. Without it the CWD is wherever this
rem     .bat lives, Resolve cannot find its own components and exits silently
rem     with no error and no window. That is what "flash and gone" looked like.
rem   * Resolve takes well over 30s to come up (615 MB exe, Fusion init).
rem     A 15s readiness check reports a false failure.
rem
rem  ASCII only: .bat is parsed with the OEM code page. Do NOT add chcp here --
rem  forcing 437 makes the Chinese project path print as "D:\??\..." which
rem  looks like a crash and hides the real message.
rem ===========================================================================
setlocal

set "RESOLVE=%ProgramFiles%\Blackmagic Design\DaVinci Resolve\Resolve.exe"
if not exist "%RESOLVE%" set "RESOLVE=D:\DaVinci Resolve\Resolve.exe"
for %%I in ("%RESOLVE%") do set "RESOLVE_DIR=%%~dpI"
set "LOG=%~dp0launch.log"
set "SYS=%SystemRoot%\System32"

echo. >> "%LOG%"
echo [%DATE% %TIME%] ---- launch attempt ---- >> "%LOG%"

echo ==========================================
echo  DaVinci Resolve + BEAUTY_GPU_PIPE=1
echo ==========================================
echo.

if not exist "%RESOLVE%" (
    echo [ERROR] Resolve.exe not found. Edit RESOLVE in this file.
    echo [ERROR] exe not found >> "%LOG%"
    goto :fail
)
echo exe: %RESOLVE%
echo exe: %RESOLVE% >> "%LOG%"

"%SYS%\tasklist.exe" /fi "imagename eq Resolve.exe" 2>nul | "%SYS%\find.exe" /i "Resolve.exe" >nul
if not errorlevel 1 (
    echo [WARN] Resolve is already running.
    echo        Quit it first, otherwise the new process only focuses the old
    echo        window and the env var never takes effect.
    echo [WARN] already running >> "%LOG%"
    goto :fail
)

set "BEAUTY_GPU_PIPE=1"
echo [1/3] BEAUTY_GPU_PIPE=%BEAUTY_GPU_PIPE%
echo [1/3] env set >> "%LOG%"

echo [2/3] Starting Resolve (this takes 30-60s, window will wait) ...
echo [2/3] starting >> "%LOG%"
start "" /D "%RESOLVE_DIR%" "%RESOLVE%"

set "WAIT=0"
:waitloop
"%SYS%\ping.exe" -n 2 127.0.0.1 >nul
set /a WAIT+=2
"%SYS%\tasklist.exe" /fi "imagename eq Resolve.exe" 2>nul | "%SYS%\find.exe" /i "Resolve.exe" >nul
if not errorlevel 1 goto :started
if %WAIT% LSS 60 goto :waitloop

echo [ERROR] Resolve did not appear within 60s. Check %LOG%
echo [ERROR] did not start in 60s >> "%LOG%"
goto :fail

:started
echo [3/3] OK - Resolve is running (detected after %WAIT%s).
echo [3/3] started after %WAIT%s >> "%LOG%"
echo.
echo Open your project, apply DaVinci Beauty to a clip with a face, and play
echo a few seconds. Then read:
echo   C:\ProgramData\DaVinciBeauty\logs\ofx.log
echo.
echo Look for these two lines (they prove the GPU path is live):
echo   [engine/I] GPU backend ready ...
echo   [engine/I] GPU pipeline roi=... upload=... total=... mismatched=0
echo.
echo This window closes itself in 8 seconds.
"%SYS%\ping.exe" -n 9 127.0.0.1 >nul
exit /b 0

:fail
echo.
echo [FAILED] See %LOG%
echo.
pause
exit /b 1
