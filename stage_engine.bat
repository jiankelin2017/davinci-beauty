@echo off
rem 把整合包自带的引擎资产放回 install.bat 认的位置。
rem 换到新机器、或 ProgramData 引擎被误删后先跑它，再跑 install.bat。
setlocal
cd /d "%~dp0"
xcopy /y /e /i "02_引擎资产" "C:\ProgramData\DaVinciBeauty\engine" >nul
if errorlevel 1 (
    echo [ERROR] stage failed
    exit /b 1
)
echo [OK] engine staged to C:\ProgramData\DaVinciBeauty\engine
echo next: 01_ofx插件源码\install.bat
