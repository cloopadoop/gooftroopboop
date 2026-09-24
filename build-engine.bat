@echo off
rem Build the Tauri sidecar gtb-engine and its driver-profile test (Release).
rem Log: build-engine.log
setlocal
cd /d "%~dp0"
type nul > build-engine.log
call "%~dp0tools\vs-env.cmd" build-engine.log
if errorlevel 1 (
  echo [build-engine] no usable MSVC environment - see build-engine.log
  exit /b 1
)
echo [build-engine] Building gtb-engine (Release)...
"%CMAKE_EXE%" --build --preset win-x64-release --target gtb-engine gtb-driver-profile-test >> build-engine.log 2>&1
if errorlevel 1 (
  echo [build-engine] BUILD FAILED - see build-engine.log
  exit /b 1
)
echo [build-engine] OK: build\win-x64\bin\gtb-engine.exe
endlocal
