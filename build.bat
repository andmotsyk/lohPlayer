@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem ---- locate MSVC -----------------------------------------------------------
rem Use !delayed! expansion everywhere VSWHERE is read: the ")" inside
rem "Program Files (x86)" closes a parenthesised block at parse time otherwise.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
  echo ERROR: vswhere.exe not found. Install Visual Studio Build Tools with the
  echo        "Desktop development with C++" workload.
  exit /b 1
)
for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo ERROR: no MSVC C++ toolset found.
  exit /b 1
)
rem vcvars64.bat itself prints a harmless "'vswhere.exe' is not recognized" to
rem stderr when the caller has delayed expansion on, so silence it and verify
rem the toolchain by looking for cl.exe instead.
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
where cl.exe >nul 2>nul
if errorlevel 1 (
  echo ERROR: cl.exe not on PATH after running vcvars64.bat.
  echo        Repair the Visual Studio "Desktop development with C++" workload.
  exit /b 1
)

if not exist build mkdir build

rem ---- icon (generated once) --------------------------------------------------
if not exist src\icon.ico (
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-icon.ps1
)

rem ---- resources --------------------------------------------------------------
rc /nologo /fo build\app.res src\app.rc
if errorlevel 1 exit /b 1

rem ---- compile + link ---------------------------------------------------------
rem /MT      static CRT      -> no VC++ redistributable, single self-contained exe
rem /GL /LTCG whole-program optimisation
rem /guard:cf control-flow guard (helps with AV/SmartScreen heuristics)
rem /utf-8   sources are UTF-8; without it MSVC reads them as the ANSI codepage
set CLFLAGS=/nologo /std:c++17 /utf-8 /O2 /Oi /GL /MT /EHsc /GS /guard:cf /DNDEBUG /DUNICODE /D_UNICODE /W3 /permissive-
set LIBS=mfplat.lib mfreadwrite.lib mfuuid.lib mf.lib ole32.lib oleaut32.lib propsys.lib ^
 shlwapi.lib shell32.lib user32.lib gdi32.lib comdlg32.lib dwmapi.lib avrt.lib advapi32.lib

cl %CLFLAGS% /Fo:build\ /Fd:build\ /Fe:build\lohPlayer.exe ^
   src\main.cpp src\audio.cpp src\decoder.cpp src\resampler.cpp src\dsp.cpp ^
   src\playlist.cpp src\config.cpp ^
   build\app.res ^
   /link /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /SUBSYSTEM:WINDOWS ^
   /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /RELEASE %LIBS%
if errorlevel 1 (
  echo.
  echo BUILD FAILED.
  exit /b 1
)

rem ---- offline guarantee: fail the build if any network import crept in -------
dumpbin /nologo /imports build\lohPlayer.exe > build\imports.txt
findstr /I /C:"WS2_32" /C:"WSOCK32" /C:"WININET" /C:"WINHTTP" /C:"URLMON" /C:"DNSAPI" /C:"IPHLPAPI" build\imports.txt >nul
if not errorlevel 1 (
  echo.
  echo *** BUILD REJECTED: a network-capable import was linked in. ***
  findstr /I /C:"WS2_32" /C:"WSOCK32" /C:"WININET" /C:"WINHTTP" /C:"URLMON" /C:"DNSAPI" /C:"IPHLPAPI" build\imports.txt
  del build\lohPlayer.exe
  exit /b 1
)

for %%F in (build\lohPlayer.exe) do set SIZE=%%~zF
echo.
echo Built build\lohPlayer.exe  (!SIZE! bytes)  - no network imports.
endlocal
