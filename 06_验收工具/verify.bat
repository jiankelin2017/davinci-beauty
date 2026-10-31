@echo off
rem DaVinci Beauty one-click project acceptance. Double-click to run.
rem exit code 0 = all PASS, 1 = has FAIL
setlocal
cd /d "%~dp0"

set PY=C:\Users\Administrator\.workbuddy\binaries\python\envs\default\Scripts\python.exe
if not exist "%PY%" set PY=python

"%PY%" acceptance.py %*
set RC=%ERRORLEVEL%

echo.
if "%RC%"=="0" (
    echo Acceptance passed.
) else (
    echo Acceptance not passed, exit %RC%. See FAIL items above.
)
pause
exit /b %RC%
