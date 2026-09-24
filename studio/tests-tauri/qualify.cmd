@echo off
setlocal
cd /d "%~dp0.."
if not defined SNSF9X_SOURCE (
  echo Set SNSF9X_SOURCE to the external CPU emulator source directory.
  exit /b 2
)
if not defined GTB_CPU_PLAYBACK_MANIFEST (
  echo Set GTB_CPU_PLAYBACK_MANIFEST to a private manifest of freshly exported ROM fixtures.
  exit /b 2
)
if not exist "%GTB_CPU_PLAYBACK_MANIFEST%" (
  echo CPU playback manifest is missing.
  exit /b 2
)
for %%I in ("%GTB_CPU_PLAYBACK_MANIFEST%") do set "GTB_CPU_PLAYBACK_MANIFEST=%%~fI"
if defined GTB_TEST_APP (
  echo Unset GTB_TEST_APP for a complete build qualification; test an isolated portable layout separately.
  exit /b 2
)
set "GTB_TEST_SKIP_ENGINE_BUILD="
set "GTB_TEST_RELEASE="
set "GTB_ENGINE_ROOT=..\gooftroopboop"
if exist ..\src\engine\EngineMain.cpp set "GTB_ENGINE_ROOT=.."
for %%I in ("%GTB_ENGINE_ROOT%") do set "GTB_ENGINE_ROOT=%%~fI"
if not exist output\testing mkdir output\testing
call :run > output\testing\qualification.log 2>&1
set "TEST_EXIT=%ERRORLEVEL%"
echo Qualification exit code: %TEST_EXIT%. Details: output\testing\qualification.log
exit /b %TEST_EXIT%

:run
call tests-tauri\all.cmd || exit /b 1
cd /d "%~dp0.."
call tests-tauri\release.cmd || exit /b 1
cd /d "%GTB_ENGINE_ROOT%"
call run-stress.cmd || exit /b 1
call tests\emulator\build.cmd || exit /b 1
set "CPU_RESULT=build\test-artifacts\cpu-%RANDOM%-%RANDOM%"
call python tests\emulator\qualify.py "%GTB_CPU_PLAYBACK_MANIFEST%" --out "%CPU_RESULT%" || exit /b 1
echo CPU evidence: %CPU_RESULT%
echo All requested local automated qualification lanes passed.
exit /b 0
