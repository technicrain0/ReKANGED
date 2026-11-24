@echo off
setlocal
rem Makes Makoto Yuki (license plate M3M3NT0) from your own Persona 3 FES disc (USA, SLUS-21621).
rem Put the .iso in the assets folder and run this, or drag the .iso onto this file.
rem TrueCrimeLA.exe does the work (--import-makoto): no other program is needed.
cd /d "%~dp0"
title True Crime: Streets of LA - Makoto Yuki from Persona 3 FES
echo.
echo   True Crime: Streets of LA - Makoto Yuki (M3M3NT0) from Persona 3 FES
echo   ====================================================================
echo.
set "EXE=%~dp0TrueCrimeLA.exe"
if not exist "%EXE%" set "EXE=%~dp0out\build\release\TrueCrimeLA.exe"
if not exist "%EXE%" (
    echo.
    echo   TrueCrimeLA.exe was not found beside this file. Build the port first ^(build.bat^).
    goto :end
)
set "ISO=%~1"
if defined ISO (
    "%EXE%" --import-makoto "%ISO%"
) else (
    "%EXE%" --import-makoto
)
:end
echo.
pause
