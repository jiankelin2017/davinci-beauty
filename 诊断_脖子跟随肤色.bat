@echo off
setlocal
title DaVinci Beauty - 脖子跟随肤色轮廓
rem 本脚本只设置一个环境变量再启动 Resolve：不改插件、不改 beauty.ini、不改工程。
rem 关掉本窗口、改用普通方式启动 Resolve，就完全恢复原状。
rem
rem BEAUTY_MASK_SHAPE 的位含义（beauty_core.cpp:2727 / 2813 / 2879）：
rem   bit0(1) = 脸框用椭圆
rem   bit1(2) = 脖子用椭圆
rem   bit2(4) = 脖子逐行跟随肤色实际边界（从中心向两侧探到肤色结束处）
rem   默认 0 = 脸框和脖子都用矩形
rem
rem 本档 = 4：脸框保持矩形，脖子改为逐行跟随肤色真实轮廓。
rem          目的是让左右边界落在皮肤/头发/衣领的真实分界上，
rem          而不是固定 0.9 脸宽的两条竖线。

set BEAUTY_MASK_SHAPE=4

rem 用完整路径调 find，避免被系统 PATH 里别的同名程序抢先（Git/MSYS 就有一个）。
"%SystemRoot%\System32\tasklist.exe" /fi "imagename eq Resolve.exe" /nh 2>nul | "%SystemRoot%\System32\find.exe" /i "Resolve.exe" >nul
if not errorlevel 1 (
    echo.
    echo   [注意] 检测到 DaVinci Resolve 正在运行。
    echo          请先完全退出 Resolve 再运行本脚本，
    echo          否则新进程会复用旧实例，环境变量不生效。
    echo.
    pause
    exit /b 1
)

if not exist "C:\ProgramData\DaVinciBeauty" mkdir "C:\ProgramData\DaVinciBeauty"
> "C:\ProgramData\DaVinciBeauty\mask_shape_run.txt" echo %DATE% %TIME% mask_shape=4 (脖子跟随肤色)

echo ============================================================
echo   DaVinci Beauty - 脖子跟随肤色轮廓
echo ============================================================
echo.
echo   本次启动参数：BEAUTY_MASK_SHAPE=4   脸=矩形 脖子=跟随肤色
echo.
echo   面板参数保持现状不用动。
echo.
echo   按任意键启动 Resolve。
pause >nul

start "" "D:\DaVinci Resolve\Resolve.exe"
echo.
echo   已启动 Resolve。请做两件事：
echo     1. 打开工程，选中出现白块的那个片段
echo     2. 播同一段十几秒，重点看脖子和胸口
echo.
echo   播完回本窗口按任意键。
pause >nul

echo.
echo ---- 本次运行标记 ----
type "C:\ProgramData\DaVinciBeauty\mask_shape_run.txt"
echo.
echo   请回报两件事：
echo     1. 白色块：完全消失 / 只剩一条横边 / 完全没变
echo     2. 脖子的磨皮和美白是否还在（确认脖子没被整块去掉）
echo.
pause
