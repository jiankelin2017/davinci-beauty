@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem ---------------------------------------------------------------
rem NamaBeauty OFX build script (no CMake required)
rem Output: out\NamaBeauty.ofx  (standard Win64 DLL)
rem ---------------------------------------------------------------

set MSVCROOT=C:\Program Files\Microsoft Visual Studio\18\Community
set MSVCVER=14.51.36231
set SDKVER=10.0.26100.0

set VCTOOLS=%MSVCROOT%\VC\Tools\MSVC\%MSVCVER%
set WINSDK=C:\Program Files ^(x86^)\Windows Kits\10

if not exist "%VCTOOLS%\bin\Hostx64\x64\cl.exe" (
    echo [ERROR] cl.exe not found under %VCTOOLS%
    echo         Edit MSVCROOT / MSVCVER in this script.
    exit /b 1
)

set INCLUDE=%VCTOOLS%\include;%WINSDK%\Include\%SDKVER%\ucrt;%WINSDK%\Include\%SDKVER%\shared;%WINSDK%\Include\%SDKVER%\um
set LIB=%VCTOOLS%\lib\x64;%WINSDK%\Lib\%SDKVER%\ucrt\x64;%WINSDK%\Lib\%SDKVER%\um\x64
set PATH=%VCTOOLS%\bin\Hostx64\x64;%PATH%

if not exist obj mkdir obj
if not exist out mkdir out

rem /arch:AVX2 -- the float32<->RGBA8 conversion is the only part of the
rem   per-frame path we fully control, so it is vectorised. Verified with
rem   IsProcessorFeaturePresent(40)=TRUE on this machine.
rem /EHsc   -- __try/__except in nama_engine.cpp needs this to coexist with
rem   the C++ objects that live in the other functions.
echo [COMPILE] NamaBeauty.ofx ...
cl /nologo /O2 /EHsc /MD /W3 /utf-8 /arch:AVX2 /std:c++17 ^
   /Foobj\ ^
   /Feout\NamaBeauty.ofx ^
   /LD ^
   /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
   /I "%~dp0sdk\include" ^
   "%~dp0src\nama_engine.cpp" ^
   "%~dp0src\ofx_plugin.cpp" ^
   /link opengl32.lib gdi32.lib user32.lib /IMPLIB:obj\NamaBeauty.lib

if errorlevel 1 (
    echo [FAILED] compile error
    exit /b 1
)

echo.
echo [OK] out\NamaBeauty.ofx
echo.
echo [EXPORTS]
dumpbin /exports out\NamaBeauty.ofx 2>nul | findstr /I "OfxGetPlugin OfxGetNumberOfPlugins NamaSelfTest"
exit /b 0
