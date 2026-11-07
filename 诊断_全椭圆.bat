@echo off
setlocal
title DaVinci Beauty - 脸框+脖子全椭圆
rem 本脚本只设置一个环境变量再启动 Resolve：不改插件、不改 beauty.ini、不改工程。
rem 关掉本窗口、改用普通方式启动 Resolve，就完全恢复原状。
rem
rem BEAUTY_MASK_SHAPE 的位含义（beauty_core.cpp:2727 / 2813 / 2879）：
rem   bit0(1) = 脸框用椭圆
rem   bit1(2) = 脖子用椭圆
rem   bit2(4) = 脖子逐行跟随肤色实际边界
rem   默认 0 = 脸框和脖子都用矩形
rem
rem 本档 = 3：脸框和脖子都用椭圆距离场，画面里不再有任何矩形直边（bit0+bit1）。
rem          改动面最大，用来判断残留的边是不是脸框矩形造成的。

set BEAUTY_MASK_SHAPE=3

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
> "C:\ProgramData\DaVinciBeauty\mask_shape_run.txt" echo %DATE% %TIME% mask_shape=3 （全椭圆）

echo ============================================================
echo   DaVinci Beauty - 脸框+脖子全椭圆
echo ============================================================
echo.
echo   本次启动参数：BEAUTY_MASK_SHAPE=3  脸框+脖子=全椭圆
echo.
echo   启动后请做一件事：
echo     用【同一段素材】播十几秒，重点看「胸部靠上」那块，
echo     确认还有没有白色长方形。
echo.
echo   看完回来告诉我，我读日志确认环境变量是否生效。
echo.

start "" "D:\DaVinci Resolve\Resolve.exe"
exit /b 0
