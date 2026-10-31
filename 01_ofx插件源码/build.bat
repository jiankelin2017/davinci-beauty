@echo off
rem Thin wrapper: the real build logic lives in build.ps1 because this repository
rem path contains non-ASCII characters and cmd.exe decodes .bat bytes with the
rem active OEM code page (GBK=936 or UTF-8=65001); a literal non-ASCII include
rem path inside a .bat used to break the ONNX header lookup. This file is pure
rem ASCII and CRLF on purpose. All arguments are forwarded to build.ps1.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %errorlevel%
