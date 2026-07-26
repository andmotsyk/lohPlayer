@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" exit /b 1
for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
where cl.exe >nul 2>nul
if errorlevel 1 ( echo ERROR: MSVC not available. & exit /b 1 )

if not exist build\b mkdir build\b
cl /nologo /std:c++17 /utf-8 /O2 /MT /EHsc /W3 /Fo:build\b\ /Fe:build\decode_bench.exe ^
   tests\decode_bench.cpp src\decoder.cpp src\resampler.cpp ^
   /link /SUBSYSTEM:CONSOLE mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib propsys.lib
if errorlevel 1 exit /b 1
build\decode_bench.exe %*
