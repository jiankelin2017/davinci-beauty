@echo off
setlocal
title DaVinci Beauty - 脖子带收短到 0.35
rem ---------------------------------------------------------------------
rem 本脚本只设置两个环境变量再启动 Resolve：不改插件、不改 beauty.ini、
rem 不改工程。关掉本窗口、改用普通方式启动，就完全恢复原状。
rem
rem BEAUTY_NECK_LEN：脖子带长度，单位是「脸高」的倍数。
rem   0.65 = 原来写死的值，带长 156 像素，下缘停在 y=616。
rem   0.90 = 上一轮测的，带长 216 像素，下缘 y=676，白块反而更大。
rem   0.35 = 本轮的，带长 84 像素，下缘约 y=544，只覆盖到颈根。
rem
rem 之所以要收短：脸框本身已经覆盖到下巴（y=460），脖子带只需要接着
rem 铺到颈根。多铺出去的那一段落在上胸，磨皮把它抹平成均匀色板，
rem 加上美白提亮，就是看到的白色长方形。
rem
rem 同时开 BEAUTY_DUMP_OUT，把掩码落盘，用来量下缘到底停在哪。
rem 不设这个变量时这段代码一次都不执行，对画面逐字节零影响。
rem ---------------------------------------------------------------------

set BEAUTY_NECK_LEN=0.35
set BEAUTY_DUMP_OUT=C:\ProgramData\DaVinciBeauty\dump

if exist "%BEAUTY_DUMP_OUT%" (
    del /q "%BEAUTY_DUMP_OUT%\dump_*.*" 2>nul
    del /q "%BEAUTY_DUMP_OUT%\analysis_*.*" 2>nul
    del /q "%BEAUTY_DUMP_OUT%\crop_*.*" 2>nul
    del /q "%BEAUTY_DUMP_OUT%\full_*.*" 2>nul
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
echo   DaVinci Beauty - 脖子带收短到 0.35
echo ============================================================
echo.
echo   本次启动：脖子带 0.65 改成 0.35 脸高（156 像素变成 84 像素）。
echo.
echo   面板参数保持 磨皮 98 / 美白 64 / 红润 30 不动，
echo   这样才和上一轮的数据可直接比。
echo.
echo   操作顺序（第 3 步很关键，上次没停对位置，导出的图是别的镜头）：
echo     1. 打开工程，选中出白块的那个片段
echo     2. 播十几秒，找到白块最明显的那一帧
echo     3. 按空格暂停，确认画面里就是那个白块，再继续往下
echo     4. 按一下左方向键，再按一下右方向键（强制重渲染并落盘）
echo     5. 等 1 秒，回本窗口按任意键
echo.
pause >nul

start "" "D:\DaVinci Resolve\Resolve.exe"

echo.
echo   已启动 Resolve。做完上面 5 步后回本窗口按任意键。
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
echo     1. 白色长方形：完全消失 / 变小了但还有边 / 没变化
echo     2. 脖子的磨皮和美白是否还在（会不会出现脸磨了、脖子没磨的色差）
echo.
pause
