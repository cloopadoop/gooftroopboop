@echo off
rem Goof Troop Boop engine regression suite.
rem Builds nothing - run build-engine.bat first if the engine changed.
rem Detail log: tests\regression.log
cd /d "%~dp0"
python tests\regression.py
exit /b %ERRORLEVEL%
