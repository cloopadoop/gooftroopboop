@echo off
setlocal
if "%~1"=="" (
  echo Usage: tests\build-driver-probe.cmd path-to-snes9x-bapu-source
  exit /b 2
)
set "PROBESOURCE=%~f1"
if not exist "%PROBESOURCE%\apu\bapu\smp\smp.cpp" exit /b 2
if not exist "%PROBESOURCE%\apu\bapu\dsp\sdsp.cpp" exit /b 2
where cl >nul 2>&1
if errorlevel 1 (
  echo Run from a Visual Studio x64 Native Tools command prompt.
  exit /b 2
)
pushd "%~dp0.."
if not exist build\driver-probe mkdir build\driver-probe
pushd build\driver-probe
cl /nologo /EHsc /std:c++17 /O2 /D__LIBRETRO__ /I"%PROBESOURCE%" /I"%PROBESOURCE%\apu\bapu" "..\..\tests\driver_probe_snes9x.cpp" "%PROBESOURCE%\apu\bapu\smp\smp.cpp" "%PROBESOURCE%\apu\bapu\dsp\sdsp.cpp" /Fe:driver-probe.exe > build.log 2>&1
set "RESULT=%ERRORLEVEL%"
popd
popd
exit /b %RESULT%
