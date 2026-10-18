@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
setlocal DisableDelayedExpansion
if "%~1"=="" (
    echo Drag your Dead or Alive Ultimate Disc 2 ISO onto BuildGame.cmd.
    pause
    exit /b 1
)
python -c "import sys; sys.exit(sys.version_info < (3, 12))" >nul 2>&1
if errorlevel 1 (
    echo Install Python 3.12 or newer from https://www.python.org/downloads/windows/
    echo Tick "Add python.exe to PATH" during setup, then try again.
    pause
    exit /b 1
)
python "%~dp0tools\build_game.py" "%~1"
set "result=%errorlevel%"
echo.
pause
exit /b %result%
