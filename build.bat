@echo off
setlocal EnableDelayedExpansion
REM ============================================================
REM  SysWidget build script
REM  Builds native x64 AND ARM64 executables from one source tree.
REM  Usage:  build.bat            (build both, release)
REM          build.bat x64        (only x64)
REM          build.bat arm64      (only arm64)
REM          build.bat debug      (both, debug symbols, no opt)
REM ============================================================

cd /d "%~dp0"

REM ---- locate Visual Studio Build Tools -------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [error] vswhere.exe not found. Install VS Build Tools with the C++ workload.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo [error] No VC++ toolset found in the installed Build Tools.
  exit /b 1
)
set "VCVARSALL=%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat"

REM ---- figure out the real host arch (survives x64 emulation on ARM64) -------
set "HOSTARCH=%PROCESSOR_ARCHITECTURE%"
if defined PROCESSOR_ARCHITEW6432 set "HOSTARCH=%PROCESSOR_ARCHITEW6432%"

REM ---- parse args -----------------------------------------------------------
set "DEBUG=0"
set "DO_X64=0"
set "DO_ARM64=0"
set "ANYARCH=0"
for %%a in (%*) do (
  if /i "%%a"=="debug"  set "DEBUG=1"
  if /i "%%a"=="x64"    ( set "DO_X64=1" & set "ANYARCH=1" )
  if /i "%%a"=="arm64"  ( set "DO_ARM64=1" & set "ANYARCH=1" )
)
if "%ANYARCH%"=="0" ( set "DO_X64=1" & set "DO_ARM64=1" )

if not exist dist  mkdir dist
if not exist build mkdir build

if "%DO_X64%"=="1"   call :build x64
if errorlevel 1 exit /b 1
if "%DO_ARM64%"=="1" call :build arm64
if errorlevel 1 exit /b 1

REM ship the default config alongside the binaries
if exist config.ini.default copy /y config.ini.default dist\config.ini.default >nul

echo.
echo [ok] done. binaries are in .\dist\
exit /b 0

REM ===========================================================================
:build
set "TARGET=%~1"

REM pick the right host_target toolset for vcvarsall
if /i "%HOSTARCH%"=="ARM64" (
  if /i "%TARGET%"=="x64"   set "TOOLSET=arm64_amd64"
  if /i "%TARGET%"=="arm64" set "TOOLSET=arm64"
) else (
  if /i "%TARGET%"=="x64"   set "TOOLSET=amd64"
  if /i "%TARGET%"=="arm64" set "TOOLSET=amd64_arm64"
)

echo.
echo === building %TARGET%  (host=%HOSTARCH%, toolset=%TOOLSET%) ===
call "%VCVARSALL%" %TOOLSET% >nul
if errorlevel 1 (
  echo [error] vcvarsall failed for %TOOLSET% ^(is the %TARGET% toolset installed?^)
  exit /b 1
)

REM compile the resource (icon + manifest) once per arch
rc /nologo /I src /I res /fo "build\res_%TARGET%.res" res\resource.rc
if errorlevel 1 exit /b 1

set "OUT=dist\SysWidget-%TARGET%.exe"

if "%DEBUG%"=="1" (
  set "CFLAGS=/nologo /W3 /Zi /Od /MTd /std:c++17 /utf-8 /EHsc /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /D_DEBUG"
  set "LFLAGS=/DEBUG /SUBSYSTEM:WINDOWS"
) else (
  set "CFLAGS=/nologo /W3 /O1 /Os /Oy /GS- /Gy /Gw /GL /MT /std:c++17 /utf-8 /EHsc /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DNDEBUG"
  set "LFLAGS=/LTCG /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MERGE:.rdata=.text"
)

cl %CFLAGS% /Fo"build\\" /Fe"%OUT%" src\*.cpp "build\res_%TARGET%.res" ^
   /link %LFLAGS% user32.lib gdi32.lib shell32.lib iphlpapi.lib ntdll.lib advapi32.lib comctl32.lib
if errorlevel 1 (
  echo [error] build failed for %TARGET%
  exit /b 1
)
echo [ok] %OUT%
exit /b 0
