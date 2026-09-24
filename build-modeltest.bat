@echo off
rem Build the headless gtb-model-test target (Release). Log: build-modeltest.log
setlocal
cd /d "%~dp0"
type nul > build-modeltest.log
call "%~dp0tools\vs-env.cmd" build-modeltest.log
if errorlevel 1 (
  echo [build-modeltest] no usable MSVC environment - see build-modeltest.log
  exit /b 1
)
echo [build-modeltest] Building gtb-model-test (Release)...
"%CMAKE_EXE%" --build --preset win-x64-release --target gtb-model-test >> build-modeltest.log 2>&1
if errorlevel 1 (
  echo [build-modeltest] BUILD FAILED - see build-modeltest.log
  exit /b 1
)
echo [build-modeltest] OK
endlocal
