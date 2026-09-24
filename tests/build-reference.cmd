@echo off
setlocal
if "%~1"=="" (
  echo Usage: tests\build-reference.cmd path-to-snes9x-bapu-source
  exit /b 2
)
set "REFSOURCE=%~f1"
if not exist "%REFSOURCE%\apu\bapu\smp\smp.cpp" exit /b 2
if not exist "%REFSOURCE%\apu\bapu\dsp\sdsp.cpp" exit /b 2
where cl >nul 2>&1
if errorlevel 1 (
  echo Run from a Visual Studio x64 Native Tools command prompt.
  exit /b 2
)
pushd "%~dp0.."
if not exist build\reference-apu mkdir build\reference-apu
pushd build\reference-apu
cl /nologo /EHsc /std:c++17 /O2 /D__LIBRETRO__ /I"%REFSOURCE%" /I"%REFSOURCE%\apu\bapu" "..\..\tests\reference_snes9x_apu.cpp" "%REFSOURCE%\apu\bapu\smp\smp.cpp" "%REFSOURCE%\apu\bapu\dsp\sdsp.cpp" /Fe:reference-apu.exe > build.log 2>&1
set "RESULT=%ERRORLEVEL%"
popd
popd
exit /b %RESULT%
