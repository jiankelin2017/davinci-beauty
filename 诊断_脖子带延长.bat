@echo off
setlocal
title DaVinci Beauty - 脖子带延长到领口
rem ---------------------------------------------------------------------
rem 本脚本只设置两个环境变量再启动 Resolve：不改插件、不改 beauty.ini、
rem 不改工程。关掉本窗口、改用普通方式启动，就完全恢复原状。
rem
rem 1) BEAUTY_NECK_LEN=0.90
rem    脖子带长度，单位是「脸高」的倍数，原来写死 0.65。
rem    0.65 时下缘停在领口上方约 45 像素的纯皮肤上，四条边都是直线，
rem    磨皮 98 把这块皮肤抹平成均匀色板，就是看到的「白色长方形」。
rem    0.90 让带子伸过领口；吊带的 G 明显大于 R，skinWeight 判成非肤色，
rem    不会落笔，于是掩码自然停在领口的弧线上，不再有水平硬边。
rem
rem 2) BEAUTY_DUMP_OUT
rem    前 6 个检出脸的帧落盘 src/dst/mask，用来量边界到底停在哪。
rem    不设这个变量时这段代码一次都不执行，对画面逐字节零影响。
rem ---------------------------------------------------------------------

set BEAUTY_NECK_LEN=0.90
set BEAUTY_DUMP_OUT=C:\ProgramData\DaVinciBeauty\dump

if exist "%BEAUTY_DUMP_OUT%" (
    del /q "%BEAUTY_DUMP_OUT%\dump_*.*" 2>nul
    del /q "%BEAUTY_DUMP_OUT%\analysis_*.*" 2>nul
    del /q "%BEAUTY_DUMP_OUT%\crop_*.*" 2>nul
)

rem 用完整路径调 find，避免被 PATH 里别的同名程序抢先（Git/MSYS 就有一个）
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

if not exist "%BEAUTY_DUMP_OUT%" mkdir "%BEAUTY_DUMP_OUT%"

echo ============================================================
echo   DaVinci Beauty - 脖子带延长到领口
echo ============================================================
echo.
echo   本次启动：脖子带 0.65 -^> 0.90 脸高，并导出 6 帧对比图。
echo.
echo   面板参数请保持 磨皮 98 / 美白 64 / 红润 30 不动，
echo   这样才和上一轮的数据可直接比。
echo.
echo   操作顺序：
echo     1. 打开工程，选中出白块的那个片段
echo     2. 播十几秒，重点看脖子到胸口那段
echo     3. 停在白块最明显的位置，按一下左方向键、再按一下右方向键
echo        （强制重新渲染当前帧，插件在这里落盘）
echo     4. 等 1 秒，回本窗口按任意键
echo.
pause >nul

start "" "D:\DaVinci Resolve\Resolve.exe"

echo.
echo   已启动 Resolve。做完上面 4 步后回本窗口按任意键。
pause >nul

echo.
echo ---- 导出的文件 ----
dir /b "%BEAUTY_DUMP_OUT%" 2>nul
echo.
if not exist "%BEAUTY_DUMP_OUT%\dump_0_src.ppm" (
    echo   [警告] 一张都没导出来。多数是 Resolve 没真正重启，环境变量没生效。
) else (
    echo   文件留在原处，直接读就行。
)
echo.
echo   请回报两件事：
echo     1. 白色长方形：消失 / 变成弧形边界 / 变大更明显 / 没变
echo     2. 胸口那片磨皮是变多了还是变少了
echo.
pause
