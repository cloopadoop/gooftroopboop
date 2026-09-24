@echo off
setlocal
cd /d "%~dp0.."

set "TEST_OUTPUT=output\tauri"
set "TEST_VENV=%TEST_OUTPUT%\venv"
set "TEST_LOG=%TEST_OUTPUT%\run.log"
if "%GTB_TEST_RELEASE%"=="1" set "TEST_LOG=%TEST_OUTPUT%\run-release.log"

if not exist "%TEST_OUTPUT%" mkdir "%TEST_OUTPUT%"
call :run > "%TEST_LOG%" 2>&1
set "TEST_EXIT=%ERRORLEVEL%"
type "%TEST_LOG%"
exit /b %TEST_EXIT%

:run
where tauri-driver >nul 2>&1 || (
  echo Missing tauri-driver. Install it with: cargo install tauri-driver --locked
  exit /b 2
)
where msedgedriver >nul 2>&1 || (
  echo Missing msedgedriver.exe. Install the version matching Microsoft Edge WebView2.
  exit /b 2
)

if not exist "%TEST_VENV%\Scripts\python.exe" (
  call python -m venv "%TEST_VENV%" || exit /b 1
)
call "%TEST_VENV%\Scripts\python.exe" -c "import selenium; assert selenium.__version__ == '4.48.0'" >nul 2>&1 || (
  call "%TEST_VENV%\Scripts\python.exe" -m pip install --disable-pip-version-check -r tests-tauri\requirements.txt || exit /b 1
)
if defined GTB_TEST_APP goto test
call "%TEST_VENV%\Scripts\python.exe" tests-tauri\sync_engine.py || exit /b 1
set "TEST_BUILD_ARGS=--debug --no-bundle"
if "%GTB_TEST_RELEASE%"=="1" set "TEST_BUILD_ARGS=--no-bundle"
call npm run tauri build -- %TEST_BUILD_ARGS% || exit /b 1
:test
call "%TEST_VENV%\Scripts\python.exe" tests-tauri\ownership_regression.py || exit /b 1
call "%TEST_VENV%\Scripts\python.exe" -m unittest discover -s tests-tauri -p "test_*.py" -v
exit /b %ERRORLEVEL%
