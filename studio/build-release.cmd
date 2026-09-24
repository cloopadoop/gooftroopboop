@echo off
rem Release build of the desktop app (frontend + Rust shell) for packaging.
rem Rust embeds source locations (panic messages) for this crate and every
rem dependency in the Cargo home, both under the user profile; remapping that
rem prefix to "~" keeps build-machine paths out of the exe.
rem Output: src-tauri\target\release\goof-troop-boop.exe. Log: build-release.log
setlocal
cd /d "%~dp0"
set "LOG=%~dp0build-release.log"
set "CARGO=cargo"
where cargo >nul 2>&1 || set "CARGO=%USERPROFILE%\.cargo\bin\cargo.exe"
set "RUSTFLAGS=%RUSTFLAGS% --remap-path-prefix=%USERPROFILE%=~"
echo [build-release] RUSTFLAGS=%RUSTFLAGS% > "%LOG%"
call npm run build >> "%LOG%" 2>&1
if errorlevel 1 (
  echo [build-release] frontend build FAILED - see build-release.log
  exit /b 1
)
pushd src-tauri
"%CARGO%" build --release >> "%LOG%" 2>&1
set "RC=%ERRORLEVEL%"
popd
if not "%RC%"=="0" (
  echo [build-release] Rust build FAILED - see build-release.log
  exit /b 1
)
echo [build-release] OK: src-tauri\target\release\goof-troop-boop.exe
endlocal
