@echo off
rem Build all targets (Debug) using the win-x64 preset. Log: build-all.log
rem Debug executables land in build\win-x64\bin\Debug; Release stays in bin.
setlocal
cd /d "%~dp0"
type nul > build-all.log
call "%~dp0tools\vs-env.cmd" build-all.log
if errorlevel 1 (
  echo [build-all] no usable MSVC environment - see build-all.log
  exit /b 1
)
echo [build-all] Building all targets Debug...
"%CMAKE_EXE%" --build --preset win-x64-debug >> build-all.log 2>&1
if errorlevel 1 (
  echo [build-all] BUILD FAILED - see build-all.log
  exit /b 1
)
echo [build-all] OK
endlocal
