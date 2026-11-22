@echo off
setlocal
rem Adds the PC version's nine extra skins (its own license plate codes) from your own PC copy of the game.
rem Drag the PC game's folder onto this file, or run it and drag the folder into the window when asked.
rem TrueCrimeLA.exe does the work (--import-pc-skins): no other program is needed.
cd /d "%~dp0"
title True Crime: Streets of LA - import the PC skins
echo.
echo   True Crime: Streets of LA - import the PC skins
echo   ===============================================
echo.
set "EXE=%~dp0TrueCrimeLA.exe"
if not exist "%EXE%" set "EXE=%~dp0out\build\release\TrueCrimeLA.exe"
if not exist "%EXE%" (
    echo.
    echo   TrueCrimeLA.exe was not found beside this file. Build the port first ^(build.bat^).
    goto :end
)
set "PCDIR=%~1"
if not defined PCDIR (
    echo   Drag your PC True Crime: Streets of LA folder ^(the one with TrueCrime.exe^) into this window,
    set /p "PCDIR=  then press Enter: "
)
if not defined PCDIR goto :end
set "PCDIR=%PCDIR:"=%"
if "%PCDIR:~-1%"=="\" set "PCDIR=%PCDIR:~0,-1%"
echo.
"%EXE%" --import-pc-skins "%PCDIR%"
:end
echo.
pause
