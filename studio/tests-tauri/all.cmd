@echo off
setlocal
cd /d "%~dp0.."
if not exist output\testing mkdir output\testing
call :run > output\testing\all.log 2>&1
set "TEST_EXIT=%ERRORLEVEL%"
type output\testing\all.log
exit /b %TEST_EXIT%

:run
set "GTB_ENGINE_ROOT=..\gooftroopboop"
if exist ..\src\engine\EngineMain.cpp set "GTB_ENGINE_ROOT=.."
echo [1/5] Build engine and run corpus regressions
if not "%GTB_TEST_SKIP_ENGINE_BUILD%"=="1" call "%GTB_ENGINE_ROOT%\build-engine.bat" || exit /b 1
cd /d "%~dp0.."
call python "%GTB_ENGINE_ROOT%\tests\regression.py" || exit /b 1
call "%GTB_ENGINE_ROOT%\build\win-x64\bin\gtb-driver-profile-test.exe" || exit /b 1
call python "%GTB_ENGINE_ROOT%\tests\asm_regression.py" --parser-only || exit /b 1
call python "%GTB_ENGINE_ROOT%\tests\asm_regression.py" --engine || exit /b 1
if not defined GTB_SOUNDBANK set "GTB_SOUNDBANK=%CD%\src-tauri\bin\soundbank.spc"
call python "%GTB_ENGINE_ROOT%\tests\source_repair_regression.py" || exit /b 1
for %%T in ("%GTB_ENGINE_ROOT%\tests\test_*.py") do if exist "%%~fT" (
  echo Running engine test: %%~nxT
  call python "%%~fT" || exit /b 1
)
echo [2/5] Rust bridge fault tests
cargo test --manifest-path src-tauri\Cargo.toml --lib || exit /b 1
echo [3/5] Browser and component tests
call npx playwright test --workers 1 --timeout 90000 || exit /b 1
echo [4/5] Real Tauri application and native dialogs
call tests-tauri\run.cmd || exit /b 1
echo [5/5] Typecheck and production frontend build
call npm run build || exit /b 1
echo All automated test lanes passed.
exit /b 0
