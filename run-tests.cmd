@echo off
rem Goof Troop Boop engine test lane: regression suite, driver profile, ASM,
rem source repair and every tests\test_*.py. Builds nothing - run
rem build-engine.bat first if the engine changed.
rem Log: tests\run-tests.log   (the regression suite also writes tests\regression.log)
setlocal
cd /d "%~dp0"
call :run > tests\run-tests.log 2>&1
set "TEST_EXIT=%ERRORLEVEL%"
type tests\run-tests.log
exit /b %TEST_EXIT%

:run
rem ffmpeg for the MP3 export test: an explicit GTB_FFMPEG wins, then the copy
rem the studio keeps beside its sidecar, then PATH (the engine's own fallback).
if not defined GTB_FFMPEG if exist "studio\src-tauri\bin\ffmpeg.exe" set "GTB_FFMPEG=%CD%\studio\src-tauri\bin\ffmpeg.exe"
if not defined GTB_SOUNDBANK set "GTB_SOUNDBANK=%CD%\..\..\Music Sources\SPC\08 Hamlet.spc"
echo [1/5] regression suite
call python tests\regression.py || exit /b 1
echo [2/5] driver profile
build\win-x64\bin\gtb-driver-profile-test.exe || exit /b 1
echo [3/5] ASM parser + engine
call python tests\asm_regression.py --parser-only || exit /b 1
call python tests\asm_regression.py --engine || exit /b 1
echo [4/5] source repair
call python tests\source_repair_regression.py || exit /b 1
echo [5/5] engine test_*.py
for %%T in ("tests\test_*.py") do (
  echo Running %%~nxT
  call python "%%~fT" || exit /b 1
)
echo All engine test lanes passed.
exit /b 0
