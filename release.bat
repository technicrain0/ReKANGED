@echo off
setlocal
rem Makes the release players download: a folder like this, and the same as a zip, both in release\
rem   TrueCrimeLA.exe       the port (no game code: the first start compiles it from the player's own game)
rem   compiler\             the recompiler, clang and lld that first start uses
rem   assets\               empty, where the player puts the .iso or the game files
rem   *.dll                 the Visual C++ runtime and the shader compiler, so a fresh Windows needs nothing more
rem   THIRD_PARTY.txt       the licences of what the port is built with
rem The first start opens the setup window: the game (detected in assets), and optionally the PC content (the
rem player's own PC copy, browsed to) and Makoto (Persona 3 FES in assets), all made inside TrueCrimeLA.exe.
rem Nothing of the game goes in. Making it needs the game in assets here only to build the executable.
set "GAME_EXE=TrueCrimeLA"
cd /d "%~dp0"
set "OUT=out\build\release"

if not exist "%OUT%\%GAME_EXE%.exe" (
    if not exist "assets\default.xbe" if not exist "assets\*.iso" if not exist "*.iso" (
        echo To make a release, the port is built first, and building needs your game in assets:
        echo   %~dp0assets
        echo The release itself will not contain any of it, its assets folder stays empty.
        goto :fail
    )
    call build.bat || goto :fail
)

rem the Visual C++ runtime beside the executable
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
for /d %%v in ("%VSDIR%\VC\Redist\MSVC\1*") do for /d %%c in ("%%v\x64\Microsoft.VC*.CRT") do set "VCRUNTIME=%%c"
if not exist "%VCRUNTIME%\vcruntime140.dll" (
    echo Visual C++ runtime not found: install Visual Studio with the C++ workload.
    goto :fail
)
rem the configure step writes where the SDK is
if not exist "%OUT%\sdk_dir.txt" cmake --preset release >nul || goto :fail
if not exist "%OUT%\sdk_dir.txt" (
    echo %OUT%\sdk_dir.txt was not written, update the SDK.
    goto :fail
)
set /p SDK=<"%OUT%\sdk_dir.txt"

rem named for the disc the executable was built from: its hooks are made for that disc's code, and it runs that disc
rem as an .iso, an xiso or extracted files (the English-only disc is a separate build)
set "SHA="
if exist "assets\default.xbe" for /f "usebackq delims=" %%h in (`powershell -NoProfile -Command "(Get-FileHash -Algorithm SHA1 -LiteralPath 'assets\default.xbe').Hash.ToLower()"`) do set "SHA=%%h"
set "NAME=%GAME_EXE%-PAL-5Languages"
if /i "%SHA%"=="ca906ba5a9459da4309df1d185683f4293c9d2ed" set "NAME=%GAME_EXE%-PAL-English"
if /i "%SHA%"=="19aa3175ed8b31454a1d40e3cae34f5fd92b5505" set "NAME=%GAME_EXE%-PAL-English"
set "PKG=release\%NAME%"

rem only the runtime DLLs the executables import; xbe_recomp.exe needs its own copy, Windows only looks beside the
rem exe it starts
del /q "%OUT%\compiler\*.dll" 2>nul
for %%f in (msvcp140 vcruntime140) do copy /y "%VCRUNTIME%\%%f.dll" "%OUT%\compiler\" >nul

echo Gathering the release in %PKG% ...
rd /s /q "%PKG%" 2>nul
mkdir "%PKG%\assets" || goto :fail
rem a note inside assets, so the folder is in the zip (an empty folder is left out by some zip tools)
(
echo Put True Crime: Streets of LA ^(Xbox, European PAL^) here:
echo   - the disc image ^(.iso, an xiso works too^), or
echo   - the extracted files: default.xbe with the game's folders beside it.
echo Then run TrueCrimeLA.exe. The first start compiles the game's code, once.
) > "%PKG%\assets\PUT YOUR GAME HERE.txt"
copy /y "%OUT%\%GAME_EXE%.exe" "%PKG%\" >nul || goto :fail
copy /y "%SDK%\THIRD_PARTY.txt" "%PKG%\" >nul
for %%f in (msvcp140 vcruntime140 vcruntime140_1) do copy /y "%VCRUNTIME%\%%f.dll" "%PKG%\" >nul
rem the shader compiler, as the Windows SDK redistributes it
for /d %%k in ("%ProgramFiles(x86)%\Windows Kits\10\Redist\D3D\x64") do if exist "%%k\d3dcompiler_47.dll" copy /y "%%k\d3dcompiler_47.dll" "%PKG%\" >nul
rem the compiler folder without its work folder: no game code goes in, the first start builds it
robocopy "%OUT%\compiler" "%PKG%\compiler" /E /XD work /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 (
    echo Copying the compiler folder failed.
    goto :fail
)
rem nothing compiled from a game may be in there
del /q "%PKG%\%GAME_EXE%.dll" 2>nul

echo Packing release\%NAME%.zip ...
del /q "release\%NAME%.zip" 2>nul
set "SEVENZIP="
where 7z >nul 2>nul && set "SEVENZIP=7z"
if not defined SEVENZIP if exist "%ProgramFiles%\7-Zip\7z.exe" set "SEVENZIP=%ProgramFiles%\7-Zip\7z.exe"
if not defined SEVENZIP if exist "%ProgramFiles(x86)%\7-Zip\7z.exe" set "SEVENZIP=%ProgramFiles(x86)%\7-Zip\7z.exe"
if defined SEVENZIP (
    pushd "%PKG%"
    "%SEVENZIP%" a -tzip "..\%NAME%.zip" * >nul
    popd
) else (
    powershell -NoProfile -Command "Compress-Archive -Path '%PKG%\*' -DestinationPath 'release\%NAME%.zip' -CompressionLevel Optimal -Force"
)
if not exist "release\%NAME%.zip" (
    echo Packing the zip failed. The folder is still there: %~dp0%PKG%
    goto :fail
)

echo.
echo Done:
echo   folder  %~dp0%PKG%
echo   zip     %~dp0release\%NAME%.zip  ^(upload this to GitHub Releases^)
echo Players unzip it, put their .iso or game files in assets and run %GAME_EXE%.exe.
echo.
pause
exit /b 0

:fail
echo.
echo The release was not made.
pause
exit /b 1
