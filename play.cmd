@echo off
cd /d "%~dp0"
python tools\run_game.py --play
if errorlevel 1 pause
