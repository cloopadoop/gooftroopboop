@echo off
setlocal
cd /d "%~dp0.."
set "GTB_TEST_RELEASE=1"
call tests-tauri\run.cmd
exit /b %ERRORLEVEL%
