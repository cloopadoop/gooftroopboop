@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
echo [build-engine] Building gtb-engine (Release)...
cmake --build --preset win-x64-release --target gtb-engine > build-engine.log 2>&1
if errorlevel 1 (
  echo [build-engine] BUILD FAILED - see build-engine.log
  exit /b 1
)
echo [build-engine] OK
endlocal
