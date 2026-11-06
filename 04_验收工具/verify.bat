@echo off
rem DaVinci Beauty one-click project acceptance. Double-click to run.
rem exit code 0 = all PASS, 1 = has FAIL
setlocal
cd /d "%~dp0"

rem 解释器：优先用环境变量 WORKBUDDY_PY 指定的路径，否则退回 PATH 里的 python。
rem 此前这里写死了某个本机绝对路径，随发布清单外发会泄露用户名与目录结构。
set PY=%WORKBUDDY_PY%
if not defined PY set PY=python

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
