@echo off
setlocal
title DaVinci Beauty 1.1.3.0 - 验证脖子羽化
rem ---------------------------------------------------------------------
rem 插件已经是 1.1.3.0（脖子羽化按带长取 35%，18 像素变 55 像素）。
rem 本脚本只做一件事：设 BEAUTY_DUMP_OUT，让插件把你看的那一帧落盘。
rem 不改插件、不改 beauty.ini、不改工程。
rem
rem 1.1.3.0 新增了「同一帧重渲染」导出通道：
rem   播放阶段导 6 帧（dump_0..5），名额用完后不再导；
rem   你在某处暂停再按左右方向键时，宿主会重新渲染同一帧，
rem   插件识别到帧号相同，就覆盖写 dump_re_src/dst/mask 三张图。
rem   所以：停在哪一帧，dump_re_* 就是那一帧，不会抓到别的镜头。
rem ---------------------------------------------------------------------

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
echo   DaVinci Beauty 1.1.3.0 - 验证脖子羽化
echo ============================================================
echo.
echo   本次改动：脖子带羽化 18 像素 -^> 55 像素，带长恢复 0.65。
echo   预期：脖子到上胸的过渡变成一片渐变，看不到直边；
echo         脖子下半仍然磨皮美白，不会再出现脸脖色差。
echo.
echo   面板参数请保持 磨皮 98 / 美白 64 / 红润 30 不动。
echo.
echo   操作顺序：
echo     1. 打开工程，选中出白块的那个片段
echo     2. 播十几秒，重点看脖子到胸口
echo     3. 停在白块最明显的那一帧，按空格暂停
echo     4. 按一下左方向键，再按一下右方向键
echo        （这一步会写出 dump_re_* 三张图，就是你停的这一帧）
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
if not exist "%BEAUTY_DUMP_OUT%\dump_re_src.ppm" (
    echo   [提示] 没有 dump_re_*。说明第 4 步没触发，或者那一帧没检出脸。
) else (
    echo   dump_re_* 就是你暂停的那一帧，位置正确。
)
echo.
echo   请回报两件事：
echo     1. 脖子到胸口的过渡：看不到边 / 还有淡淡的边 / 仍有明显直线
echo     2. 脸和脖子的色差：没有了 / 还有一点
echo.
pause
