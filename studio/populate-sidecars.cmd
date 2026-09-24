@echo off
rem ============================================================================
rem populate-sidecars.cmd - copy exactly what the release engine needs into
rem src-tauri\bin (git-ignored; required before cargo/tauri build, which
rem bundles these files as the app's bin\ resources).
rem
rem   populate-sidecars.cmd [destination]      default: src-tauri\bin
rem
rem Sources: the engine's build\win-x64\bin (Ninja Multi-Config release build;
rem ..\build when this folder sits inside the engine repo) plus the VC++ runtime the engine and Qt6Core link against. ffmpeg
rem is optional and not copied: the engine falls back to ffmpeg on PATH.
rem Logs to sidecars.log; any missing required file fails the run.
rem ============================================================================
setlocal EnableExtensions
cd /d "%~dp0"
set "LOG=%~dp0sidecars.log"
set "ENGINE_BIN=%~dp0..\gooftroopboop\build\win-x64\bin"
if exist "%~dp0..\src\engine\EngineMain.cpp" set "ENGINE_BIN=%~dp0..\build\win-x64\bin"
set "SPC_FALLBACK=%~dp0..\..\Music Sources\SPC\08 Hamlet.spc"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "DEST=%~1"
if "%DEST%"=="" set "DEST=%~dp0src-tauri\bin"
set "MISSING=0"

echo [%date% %time%] populate-sidecars to "%DEST%" > "%LOG%"
echo engine build: "%ENGINE_BIN%" >> "%LOG%"
if not exist "%DEST%\" mkdir "%DEST%" >> "%LOG%" 2>&1
if not exist "%DEST%\" (
  echo ERROR: cannot create "%DEST%" >> "%LOG%"
  goto :fail
)

rem Release engine and the only Qt library it links.
call :copy "%ENGINE_BIN%\gtb-engine.exe" gtb-engine.exe
call :copy "%ENGINE_BIN%\Qt6Core.dll" Qt6Core.dll
rem Generic MIDI import helper: the multi-config build writes it to the
rem configuration folder, and only the Release one ships (no Qt dependency).
call :copy "%ENGINE_BIN%\Release\GTBoop-cli.exe" GTBoop-cli.exe
rem Default song: one staged next to the engine build wins, else Hamlet.
if exist "%ENGINE_BIN%\soundbank.spc" (
  call :copy "%ENGINE_BIN%\soundbank.spc" soundbank.spc
) else (
  call :copy "%SPC_FALLBACK%" soundbank.spc
)

rem VC++ runtime (gtb-engine, GTBoop-cli and Qt6Core import these).
call :find_vcrt
if not defined VCRT (
  echo ERROR: VC++ runtime DLLs not found in a Visual Studio redist or System32 >> "%LOG%"
  set "MISSING=1"
) else (
  for %%F in (msvcp140.dll msvcp140_1.dll vcruntime140.dll vcruntime140_1.dll) do call :copy "%VCRT%\%%F" %%F
)

if not "%MISSING%"=="0" goto :fail
echo OK >> "%LOG%"
echo OK - sidecars in "%DEST%" (see %LOG%)
exit /b 0

:fail
echo FAILED >> "%LOG%"
echo FAILED - see %LOG%
type "%LOG%"
exit /b 1

rem ---- helpers -----------------------------------------------------------------
rem copy <source> <name>: copy into DEST as name, logging timestamp and size.
:copy
if not exist "%~1" (
  echo MISSING: %~1 >> "%LOG%"
  set "MISSING=1"
  exit /b 1
)
copy /y "%~1" "%DEST%\%~2" >nul 2>> "%LOG%"
if errorlevel 1 (
  echo COPY FAILED: %~1 >> "%LOG%"
  set "MISSING=1"
  exit /b 1
)
echo copied %~2 ^<- %~1 [%~t1, %~z1 bytes] >> "%LOG%"
exit /b 0

rem find_vcrt: x64 CRT redist of the newest Visual Studio install (vswhere
rem -sort lists newest first; -latest can pick a VS-shell product without
rem one), else System32.
:find_vcrt
set "VCRT="
rem vswhere's path contains "(x86)", which breaks cmd's parenthesized blocks
rem and for /f commands, so its answer goes through a temp file.
set "VCRT_LIST=%TEMP%\gtb-vcrt-%RANDOM%.txt"
if not exist "%VSWHERE%" goto :find_vcrt_system
"%VSWHERE%" -all -sort -products * -find "VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" > "%VCRT_LIST%" 2>> "%LOG%"
for /f "usebackq delims=" %%D in ("%VCRT_LIST%") do if not defined VCRT if exist "%%D\vcruntime140_1.dll" set "VCRT=%%D"
del "%VCRT_LIST%" 2>nul
:find_vcrt_system
if not defined VCRT if exist "%SystemRoot%\System32\vcruntime140_1.dll" set "VCRT=%SystemRoot%\System32"
if defined VCRT echo VC++ runtime from: "%VCRT%" >> "%LOG%"
exit /b 0
