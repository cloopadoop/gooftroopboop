@echo off
rem Build GTBoop-cli (Debug) using the win-x64 preset. Writes log to build-cli.log.
setlocal
cd /d "%~dp0"
type nul > build-cli.log
call "%~dp0tools\vs-env.cmd" build-cli.log
if errorlevel 1 (
  echo [build-cli] no usable MSVC environment - see build-cli.log
  exit /b 1
)
rem Reconfigure when there is no cache, or the cache belongs to another checkout
rem (it records the source directory it was generated from).
set NEED_CONFIG=0
set "CACHED_HOME="
if not exist build\win-x64\CMakeCache.txt set NEED_CONFIG=1
for /f "tokens=1,* delims==" %%A in ('findstr /b /c:"CMAKE_HOME_DIRECTORY:" build\win-x64\CMakeCache.txt 2^>nul') do set "CACHED_HOME=%%B"
set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"
set "HERE=%HERE:\=/%"
if defined CACHED_HOME if /i not "%CACHED_HOME%"=="%HERE%" set NEED_CONFIG=1
if "%NEED_CONFIG%"=="1" (
  echo [build-cli] Configuring preset win-x64 fresh...
  "%CMAKE_EXE%" --preset win-x64 --fresh >> build-cli.log 2>&1
  if errorlevel 1 (
    echo [build-cli] CONFIGURE FAILED - see build-cli.log
    exit /b 1
  )
)
echo [build-cli] Building GTBoop-cli (Debug)...
"%CMAKE_EXE%" --build --preset win-x64-debug --target GTBoop-cli >> build-cli.log 2>&1
if errorlevel 1 (
  echo [build-cli] BUILD FAILED - see build-cli.log
  exit /b 1
)
echo [build-cli] OK: build\win-x64\bin\Debug\GTBoop-cli.exe
endlocal
