@echo off
setlocal DisableDelayedExpansion
for %%I in ("%~dp0..") do set "review_root=%%~fI"
if not exist "%review_root%\install-windows\bambu-studio.exe" (
    echo Build and install the Windows application into install-windows first.
    pause
    exit /b 1
)
cd /d "%review_root%"
start "" "%review_root%\install-windows\bambu-studio.exe" --datadir "%review_root%\test-profile"
