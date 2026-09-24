@echo off
rem Build all targets (Release) using the win-x64 preset. Log: build-all-release.log
setlocal
cd /d "%~dp0"
type nul > build-all-release.log
call "%~dp0tools\vs-env.cmd" build-all-release.log
if errorlevel 1 (
  echo [build-all-release] no usable MSVC environment - see build-all-release.log
  exit /b 1
)
echo [build-all-release] Building all targets Release...
"%CMAKE_EXE%" --build --preset win-x64-release >> build-all-release.log 2>&1
if errorlevel 1 (
  echo [build-all-release] BUILD FAILED - see build-all-release.log
  exit /b 1
)
echo [build-all-release] OK
endlocal
