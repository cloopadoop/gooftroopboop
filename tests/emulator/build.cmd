@echo off
setlocal
if not defined SNSF9X_SOURCE (
  echo Set SNSF9X_SOURCE to the external directory containing SNESSystem.cpp.
  exit /b 2
)
if not defined CMAKE_EXE set "CMAKE_EXE=cmake"
if not exist "%~dp0build" mkdir "%~dp0build"
"%CMAKE_EXE%" -S "%~dp0." -B "%~dp0build" -A x64 >"%~dp0build\configure.log" 2>&1
if errorlevel 1 exit /b 1
"%CMAKE_EXE%" --build "%~dp0build" --config Release --parallel 6 >"%~dp0build\build.log" 2>&1
exit /b %errorlevel%
