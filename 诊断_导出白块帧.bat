@echo off
setlocal
title DaVinci Beauty - 导出白块帧
rem ---------------------------------------------------------------------
rem 本脚本只设置一个环境变量再启动 Resolve：不改插件、不改 beauty.ini、
rem 不改工程。关掉本窗口、改用普通方式启动，就完全恢复原状。
rem
rem BEAUTY_DUMP_OUT 指向的目录会收到 6 组图（只导前 6 个检出脸的帧）：
rem   dump_N_src.ppm   处理之前的输入
rem   dump_N_dst.ppm   插件输出
rem   dump_N_mask.pgm  本帧掩码（脸 + 脖子，0..255）
rem 三张同尺寸、同朝向（已按上下翻转标志翻正），可以直接叠图比。
rem 不设这个变量时这段代码一次都不执行，对画面逐字节零影响。
rem ---------------------------------------------------------------------

set BEAUTY_DUMP_OUT=C:\ProgramData\DaVinciBeauty\dump

rem 清掉上一次的图，免得新旧混在一起（只删本工具自己产生的两种前缀）
if exist "%BEAUTY_DUMP_OUT%" (
    del /q "%BEAUTY_DUMP_OUT%\dump_*.*" 2>nul
    del /q "%BEAUTY_DUMP_OUT%\analysis_*.*" 2>nul
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
echo   DaVinci Beauty - 导出白块帧
echo ============================================================
echo.
echo   启动后按这个顺序操作，才能保证抓到白块那一帧：
echo.
echo     1. 打开工程，选中出白块的片段
echo     2. 播 十几秒，找到白块最明显的位置，按空格暂停
echo     3. 按一下 左方向键，再按一下 右方向键
echo        （强制重新渲染当前帧，插件会在这里落盘）
echo     4. 等 1 秒，再回来按任意键
echo.
echo   面板参数保持现状不用动。
echo.
pause >nul

start "" "D:\DaVinci Resolve\Resolve.exe"

echo.
echo   已启动 Resolve。按上面 4 步做完后回本窗口按任意键。
pause >nul

echo.
echo ---- 导出的文件 ----
dir /b "%BEAUTY_DUMP_OUT%" 2>nul
echo.
if not exist "%BEAUTY_DUMP_OUT%\dump_0_src.ppm" (
    echo   [警告] 一张都没导出来。可能是：
    echo          - 播的这段里没有检出人脸
    echo          - 面板参数全 0（引擎提前返回，没有掩码）
    echo          - Resolve 没真正重启（环境变量没生效）
) else (
    echo   文件都留在原处即可，直接读就行。
)
echo.
pause
