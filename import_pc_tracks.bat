@echo off
setlocal
rem Adds the PC version's extra soundtrack (songs 084 to 115) from your own PC copy of the game.
rem Drag the PC game's folder onto this file, or run it and drag the folder into the window when asked.
cd /d "%~dp0"
title True Crime: Streets of LA - import the PC soundtrack
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\import_pc_tracks.ps1" -PcDir "%~1" -Assets "%~dp0assets"
echo.
pause
