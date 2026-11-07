@echo off
chcp 936 >nul
setlocal
title DaVinci Beauty - 关闭脖子通道
rem 本脚本只设置一个环境变量再启动 Resolve：不改插件、不改 beauty.ini、不改工程。
rem 关掉本窗口、改用普通方式启动 Resolve，就完全恢复原状。
rem
rem BEAUTY_NECK_LEN = 脖子通道长度（脸高的倍数），默认 0.65。
rem 脖子通道从下颌线往下铺 0.65 脸高、横向 0.9 脸宽，
rem 是掩码里唯一会伸出脸框、盖到胸口和衣领的那一块。
rem
rem 本档 = 0：长度归零，等于完全不画脖子，掩码只剩脸框。
rem 用途：判断脖子附近那块色块到底落在脖子通道里，还是落在脸框内。

set BEAUTY_NECK_LEN=0

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
> "C:\ProgramData\DaVinciBeauty\neck_off_run.txt" echo %DATE% %TIME% neck_len=0 (关掉脖子通道)

echo ============================================================
echo   DaVinci Beauty - 关闭脖子通道
echo ============================================================
echo.
echo   本次启动参数：BEAUTY_NECK_LEN=0   脖子通道=关闭
echo.
echo   面板参数保持你现在的设置不用动（磨皮 0 / 美白 64 / 红润 30）。
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
type "C:\ProgramData\DaVinciBeauty\neck_off_run.txt"
echo.
echo   请回报两件事：
echo     1. 白色块：完全消失 / 变小变窄 / 完全没变
echo     2. 脸的磨皮和美白是否还在（确认脚本生效）
echo.
pause
