@echo off
setlocal
title DaVinci Beauty 1.1.7.0 - 验证下缘跟随肤色
rem ---------------------------------------------------------------------
rem 插件已经是 1.1.7.0。本版改的是「下缘判据的来源」，不是羽化、也不是几何幅值：
rem 脖子带下缘改成「逐列跟随肤色末端」（beauty.ini 的 neck_skinend，默认 1）。
rem
rem 为什么还要改：1.1.6.0 的弧其实生成了 —— 真图量到下缘 577→670→602、矢高 93 px、
rem 最长平直段只剩 19 列（原来 120 列）。但中间 186 列只降 35 px，看着还是直线；
rem 而且那条线切在锁骨下的一小块皮肤上，吊带边还在它下面约 20 px。线的两边是
rem 同一块皮肤，弧弯得再漂亮也还是能看出分界。
rem
rem 本版做法：每一列从下巴往下走，连续 3 行不是肤色就算这一列皮肤结束，下缘钉在
rem 最后一个肤色行上；同时不超过上一版的弧（只缩不扩，不会盖到衣服上出白块）。
rem 羽化按新下缘重算，分界因此贴住皮肤和衣物的交界 —— 那儿本来就有明暗纹理变化。
rem 回退：beauty.ini 的 neck_skinend 改成 0，就完全回到 1.1.6.0 的行为。
rem 局限：裸肩 / 深 V 这类没有衣物交界的镜头，会退回上一版的弧，效果与 1.1.6.0 相同。
rem
rem 取证通道沿用 1.1.5.0 的三级兜底（环境变量 > beauty.ini 的 dump_enable > 默认目录），
rem 本脚本仍会自动把 dump_enable 置 1、结束时改回 0。
rem
rem 改了什么：
rem   1. dump 开关从「只认环境变量」改成三级兜底：
rem      环境变量 BEAUTY_DUMP_OUT > beauty.ini 的 dump_enable > 默认目录
rem   2. 重渲染判据补了一条「与上一帧间隔超过 400 毫秒」，
rem      因为上一轮日志里一条「诊断导出」都没有，说明只靠帧号相同没触发
rem   3. dumpDir 为空时不再硬拼路径，改为打一行明确的日志
rem
rem 为什么要落到 ini：1.1.4.0 那次验证空手而归，根因是 Resolve 从桌面图标
rem 启动时 bat 里 set 的变量继承不到，dumpDir 成了空指针，而重渲染分支没先
rem 判空，路径被拼成 \"(null)\dump_re_src.ppm\"，写失败还不报错。
rem 这和 2026-11-06 BEAUTY_GPU_PIPE 那次是同一个坑 —— 开关必须落在磁盘上，
rem 不能挂在「你从哪个图标启动」上。
rem ---------------------------------------------------------------------

set "DUMP_OUT=C:/ProgramData/DaVinciBeauty/dump"
set "BASE=%~dp0"
set "SWITCH=%BASE%04_验收工具\dump_switch.py"

if exist "%DUMP_OUT%" (
    del /q "%DUMP_OUT%\dump_*.*" 2>nul
    del /q "%DUMP_OUT%\analysis_*.*" 2>nul
    del /q "%DUMP_OUT%\crop_*.*" 2>nul
    del /q "%DUMP_OUT%\full_*.*" 2>nul
)

rem 用绝对路径调 find，避免被 PATH 里的同名工具抢先（Git/MSYS 会带一份）
"%SystemRoot%\System32\tasklist.exe" /fi "imagename eq Resolve.exe" /nh 2>nul | "%SystemRoot%\System32\find.exe" /i "Resolve.exe" >nul
if not errorlevel 1 (
    echo.
    echo   [注意] 检测到 DaVinci Resolve 正在运行。
    echo          请先完全退出 Resolve，再运行本脚本。
    echo          否则新进程会复用旧实例，新插件与开关都可能不生效。
    echo.
    pause
    exit /b 1
)

if not exist "%DUMP_OUT%" mkdir "%DUMP_OUT%"

echo ============================================================
echo   DaVinci Beauty 1.1.7.0 - 验证下缘跟随肤色
echo ============================================================
echo.
echo   本次改动：下缘改成跟随肤色末端（neck_skinend 1），羽化与弧的幅度都不动。
echo   目的：看那条横线是否消失、分界是不是贴到了衣领/吊带上、脖子两侧有没有色差。
echo.
echo   面板请保持 磨皮 98 / 美白 64 / 红润 30 不变。
echo.
echo   操作顺序
echo     1. 先打开 Resolve，选中出白块的那个片段
echo     2. 播十几秒，重点看脖子到胸口
echo     3. 停在白块最明显的那一帧，按空格暂停
echo     4. 按一下左方向键，再按一下右方向键
echo        （这一步会写出 dump_re_*，就是你停的那一帧）
echo     5. 等 1 秒，回来按任意键
echo.
echo   注意：本脚本会把 beauty.ini 的 dump_enable 临时改成 1，
echo         结束时会自动改回 0。若中途强行关窗口，请手动改回 0。
echo.
pause >nul

rem 两条路同时铺：环境变量优先，ini 兜底。任一生效即可。
set "BEAUTY_DUMP_OUT=%DUMP_OUT%"
if exist "%SWITCH%" (
    python "%SWITCH%" on
) else (
    echo   [提示] 找不到 dump_switch.py，只能靠环境变量，可能不生效。
)

start "" "D:/DaVinci Resolve/Resolve.exe"

echo.
echo   已启动 Resolve。请按上面的步骤操作，完成后回到本窗口按任意键。
pause >nul

echo.
echo ---- 已经生成的图 ----
dir /b "%DUMP_OUT%" 2>nul
echo.
if not exist "%DUMP_OUT%\dump_re_mask.pgm" (
    echo   [提示] 没有 dump_re_*。请打开
    echo          C:/ProgramData/DaVinciBeauty/logs/ofx.log
    echo          搜「诊断导出」，按下面三种情况判断：
    echo            搜到「未启用」   -^> 开关没生效，重启 Resolve 再来一次
    echo            搜到 dump_re 行  -^> 图写成功了，路径以日志里那行为准
    echo            一条都没有       -^> 触发信号没到，暂停后多按几次左/右
) else (
    echo   dump_re_* 已生成，就是你停下的那一帧。
)

echo.
echo ---- 恢复 ini 开关 ----
if exist "%SWITCH%" python "%SWITCH%" off

echo.
echo   请回报以下两项：
echo     1. 脖子到胸口的过渡：看不到边 / 还有淡淡的边 / 仍有明显直线
echo     2. 分界位置：贴在衣领上 / 仍在锁骨下的皮肤上 / 看不出来
echo     3. 脸脖色差：没有了 / 还有一点
echo.
pause
