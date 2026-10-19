@echo off
cd /d "%~dp0"
if "%~1"=="" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0tools\launcher.ps1"
    exit /b
)
python tools\run_game.py --play --widescreen --msaa 32 --smaa %*
if errorlevel 1 pause
