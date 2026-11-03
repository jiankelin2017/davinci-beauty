@echo off
setlocal
title DaVinci Beauty - 抓取真实帧诊断
rem 本脚本用环境变量打开插件的检测诊断，把「真正送进人脸检测的像素」落盘。
rem 只写环境变量，不改注册表、不改插件、不改工程。

set BEAUTY_DEBUG_DETECT=1
set BEAUTY_DEBUG_DUMP=C:\beauty_dump
set BEAUTY_DEBUG_LOG=C:\beauty_dump\detect.log

if not exist "C:\beauty_dump" mkdir "C:\beauty_dump"
del /q "C:\beauty_dump\*.raw" 2>nul
del /q "C:\beauty_dump\*.f32" 2>nul
del /q "C:\beauty_dump\dump.txt" 2>nul
del /q "C:\beauty_dump\detect.log" 2>nul

echo ============================================================
echo   DaVinci Beauty - 抓取真实帧诊断
echo ============================================================
echo.
echo   请先【完全退出】DaVinci Resolve，再继续。
echo   本窗口不要关。
echo.
echo   按任意键启动 Resolve。
pause >nul

start "" "D:\DaVinci Resolve\Resolve.exe"
echo.
echo   已启动 Resolve。请依次做三件事：
echo     1. 打开出问题的工程
echo     2. 把播放头停到 00:01:04:08 - 也就是导出截图那一帧
echo     3. 在监视器上播放 2 秒，让插件至少渲染一帧
echo.
echo   做完后回到本窗口按任意键。
pause >nul

echo.
echo ---- 抓取结果 ----
if exist "C:\beauty_dump\src.raw" goto have_src
echo   [缺失] src.raw 没有生成 - 插件这一次没跑到真实帧检测。
goto after_src
:have_src
echo   [OK] 抓到输入帧 src.raw
for %%F in ("C:\beauty_dump\src.raw") do echo        大小 = %%~zF 字节，1080x1920 应为 8294400
:after_src
if not exist "C:\beauty_dump\in.f32" goto after_in
echo   [OK] 抓到模型输入 in.f32
:after_in
if not exist "C:\beauty_dump\dump.txt" goto after_txt
echo.
echo ---- dump.txt ----
type "C:\beauty_dump\dump.txt"
:after_txt
if not exist "C:\beauty_dump\detect.log" goto after_log
echo.
echo ---- detect.log ----
type "C:\beauty_dump\detect.log"
:after_log
echo.
echo   请把上面的输出、以及 C:\beauty_dump 目录里的文件发给我。
pause
