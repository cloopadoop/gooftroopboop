@echo off
rem vs-env.cmd - MSVC x64 environment + CMAKE_EXE for this repo's build scripts.
rem
rem   call "%~dp0tools\vs-env.cmd" "<log file>" || exit /b 1
rem
rem Uses the Visual Studio install the existing CMake cache was configured
rem with, so vcvars and the cached compiler come from one toolset; otherwise
rem the first VS 2022 install found. vcvars output goes to the log instead of
rem nul, and the call fails if cl.exe is not usable afterwards.
rem CMAKE_EXE is the cmake that generated the cache (a bare `cmake` can resolve
rem to another install, e.g. devkitPro's msys2 copy, with a different version).
rem Variables are left set for the caller (no setlocal).

set "VSENV_LOG=%~1"
if "%VSENV_LOG%"=="" set "VSENV_LOG=nul"
set "VSENV_CACHE=%~dp0..\build\win-x64\CMakeCache.txt"
set "VCVARS="
set "CMAKE_EXE="
set "VSENV_CL="
set "VSENV_CMAKE="

if exist "%VSENV_CACHE%" (
  for /f "tokens=1,* delims==" %%A in ('findstr /b /c:"CMAKE_CXX_COMPILER:" "%VSENV_CACHE%"') do set "VSENV_CL=%%B"
  for /f "tokens=1,* delims==" %%A in ('findstr /b /c:"CMAKE_COMMAND:" "%VSENV_CACHE%"') do set "VSENV_CMAKE=%%B"
)

rem ...\<VS root>\VC\Tools\MSVC\<ver>\bin\Hostx64\x64\cl.exe -> <VS root>
if defined VSENV_CL (
  set "VSENV_CL=%VSENV_CL:/=\%"
  call :vsroot_from_cl
)

if not defined VCVARS (
  for %%E in (Community Professional Enterprise BuildTools) do (
    if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
      set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
    )
    if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
      set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
    )
  )
)
if not defined VCVARS (
  echo [vs-env] no Visual Studio 2022 vcvars64.bat found>> "%VSENV_LOG%"
  echo [vs-env] no Visual Studio 2022 vcvars64.bat found
  exit /b 1
)

rem vcvars can leave a background helper holding its output handle open, so it
rem gets its own log; sharing the caller's log made the next append fail.
set "VSENV_VCLOG=%~dp0..\build\vs-env-vcvars.log"
echo [vs-env] vcvars: %VCVARS% ^(output: %VSENV_VCLOG%^)>> "%VSENV_LOG%"
call "%VCVARS%" > "%VSENV_VCLOG%" 2>&1
where cl >nul 2>&1
if errorlevel 1 (
  echo [vs-env] cl.exe is not on PATH after "%VCVARS%" - see %VSENV_VCLOG%>> "%VSENV_LOG%"
  echo [vs-env] cl.exe is not on PATH after vcvars - see %VSENV_VCLOG%
  exit /b 1
)

if defined VSENV_CMAKE set "CMAKE_EXE=%VSENV_CMAKE:/=\%"
if defined CMAKE_EXE if not exist "%CMAKE_EXE%" set "CMAKE_EXE="
if not defined CMAKE_EXE if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
if not defined CMAKE_EXE (
  echo [vs-env] no cmake.exe found ^(looked in the CMake cache and Program Files^)>> "%VSENV_LOG%"
  echo [vs-env] no cmake.exe found
  exit /b 1
)
echo [vs-env] cmake: %CMAKE_EXE%>> "%VSENV_LOG%"
exit /b 0

:vsroot_from_cl
set "VSENV_ROOT=%VSENV_CL:\VC\Tools\=|%"
for /f "tokens=1 delims=|" %%R in ("%VSENV_ROOT%") do set "VSENV_ROOT=%%R"
if exist "%VSENV_ROOT%\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%VSENV_ROOT%\VC\Auxiliary\Build\vcvars64.bat"
exit /b 0
