@echo off
setlocal
cd /d "%~dp0"
call build-modeltest.bat || exit /b 1
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cmake --build --preset win-x64-debug --target GTBoop-cli > build-cli.log 2>&1 || exit /b 1
call python tests\stress.py
exit /b %ERRORLEVEL%
