@echo off
setlocal
set "GAME_TITLE=True Crime: Streets of LA"
set "GAME_XBE=default.xbe"
set "GAME_EXE=TrueCrimeLA"
rem the European (PAL) default.xbe this port is made from: the disc as dumped (Redump .iso, or its extracted files)
set "GAME_SHA1=7c665c36d4883e087d949d0b6961dd6465484456"
rem the same xbe as found in common xiso copies of that disc: one byte of the Xbox start-up code at 0x1EAFB9 differs
rem (the jump past its start-up check), the game's own code is identical, and the port runs the same either way
set "GAME_SHA1_XISO=5e0aeb15d63d2322209013df3b13b14545ac2d7b"
rem the English-only European disc: its own build of the game (config\game_en.toml, picked by CMakeLists.txt),
rem as dumped (Redump .iso) and as found in xiso copies (the same start-up byte differs)
set "GAME_SHA1_EN=ca906ba5a9459da4309df1d185683f4293c9d2ed"
set "GAME_SHA1_EN_XISO=19aa3175ed8b31454a1d40e3cae34f5fd92b5505"
title %GAME_TITLE% - Build
cd /d "%~dp0"
echo.
echo   %GAME_TITLE% - build
echo   ================================================
echo.

set "PF86=%ProgramFiles(x86)%"
set "VSINST=%PF86%\Microsoft Visual Studio\Installer"
set "VSWHERE=%VSINST%\vswhere.exe"
set "OUT=%CD%\out\build\release"

rem game files: an .iso in assets is extracted there first (default.xbe and the game's folders)
if not exist assets\%GAME_XBE% call :extract || goto :fail
if not exist assets\%GAME_XBE% goto :nogame
call :checkxbe || goto :fail

rem toolchain
echo [1/3] Toolchain
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo       Visual Studio with the C++ workload was not found.
    echo       Install Visual Studio Community and tick "Desktop development with C++".
    goto :fail
)
call :setuptools
rem CI is set on GitHub Actions: no prompts there.
if not defined TOOLS_OK if defined CI goto :byhand
if not defined TOOLS_OK (
    choice /c YN /n /m "      Let Visual Studio install the missing components? [Y/N] "
    if errorlevel 2 goto :byhand
    echo       Close Visual Studio if it is open, it will be shut down anyway.
    echo       Installing. Accept the admin prompt, then wait for the installer.
    powershell -NoProfile -Command "Start-Process -Verb RunAs -Wait -FilePath '%VSINST%\setup.exe' -ArgumentList 'modify','--installPath','%VSDIR%','--add','Microsoft.VisualStudio.Component.VC.Llvm.Clang','--add','Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset','--add','Microsoft.VisualStudio.Component.VC.CMake.Project','--passive','--norestart','--force'"
    echo       Press a key once the Visual Studio Installer window has closed.
    pause >nul
    call :setuptools
    if not defined TOOLS_OK goto :byhand
)
for /f "tokens=*" %%v in ('clang++ --version ^| findstr /i version') do echo       %%v

rem symbols
echo [2/3] XDK symbols
cmake --preset release >nul || goto :fail
findstr /b /l /c:"[symbols]" config\game.toml >nul 2>nul && echo       config\game.toml already written && goto :game
cmake --build out\build\release --target xbe_symbols >nul || goto :fail
rem the configure step wrote where the SDK is
if not exist out\build\release\sdk_dir.txt echo       out\build\release\sdk_dir.txt was not written, update the SDK. && goto :fail
set /p SDK=<out\build\release\sdk_dir.txt
if not exist config mkdir config
"%OUT%\xbe_symbols.exe" assets\%GAME_XBE% "%SDK%\xdk" --library %GAME_EXE%.dll --out config\game.toml || goto :fail

rem game
:game
echo [3/3] Game code
echo       first build recompiles %GAME_XBE%, expect 30 to 90 minutes ...
cmake --build out\build\release || goto :fail

echo.
echo   %GAME_TITLE% built.
echo   out\build\release\%GAME_EXE%.exe
echo.
if defined CI exit /b 0
choice /c YN /n /m "  Play now? [Y/N] "
if not errorlevel 2 (start "" /d "%OUT%" "%OUT%\%GAME_EXE%.exe") else (pause)
exit /b 0

rem helpers
:setuptools
set TOOLS_OK=
set MISSING=
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%VSDIR%\VC\Tools\Llvm\x64\bin;%PATH%"
if exist "%ProgramFiles%\LLVM\bin\clang++.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
where clang++ >nul 2>nul || set MISSING=%MISSING% clang++
where cmake   >nul 2>nul || set MISSING=%MISSING% cmake
where ninja   >nul 2>nul || set MISSING=%MISSING% ninja
where git     >nul 2>nul || set MISSING=%MISSING% git
if defined MISSING (
    echo       missing:%MISSING%
    exit /b 0
)
set TOOLS_OK=1
exit /b 0

:byhand
echo.
echo       Install by hand, then run this again:
echo         clang++ / cmake / ninja  -  Visual Studio Installer, Modify, Individual
echo                                     components: "C++ Clang tools for Windows"
echo                                     and "C++ CMake tools for Windows"
echo         git                      -  https://git-scm.com/download/win
goto :fail

:extract
set "ISO="
rem in assets, or beside build.bat
for %%f in (assets\*.iso) do if not defined ISO set "ISO=%%f"
for %%f in (*.iso) do if not defined ISO set "ISO=%%f"
if not defined ISO exit /b 0
echo [0/3] Game files
echo       extracting "%ISO%" into assets, this takes a few minutes ...
rem no ( ) blocks below: the .iso's name usually has brackets, "(Europe)", which would end such a block early
powershell -NoProfile -ExecutionPolicy Bypass -File "%CD%\scripts\extract_iso.ps1" -Iso "%CD%\%ISO%" -Out "%CD%\assets"
if errorlevel 1 goto :extractfailed
if not exist assets\%GAME_XBE% goto :extractnoxbe
echo       done. The .iso can be deleted now, the extracted files are used.
echo.
exit /b 0
:extractfailed
echo       The .iso could not be extracted. Is it a complete Xbox disc image?
exit /b 1
:extractnoxbe
echo       The .iso has no %GAME_XBE%.
exit /b 1

:checkxbe
set "SHA="
for /f "usebackq delims=" %%h in (`powershell -NoProfile -Command "(Get-FileHash -Algorithm SHA1 -LiteralPath 'assets\%GAME_XBE%').Hash.ToLower()"`) do set "SHA=%%h"
if /i "%SHA%"=="%GAME_SHA1%" exit /b 0
if /i "%SHA%"=="%GAME_SHA1_XISO%" echo       default.xbe is the European release, from an xiso copy.
if /i "%SHA%"=="%GAME_SHA1_XISO%" exit /b 0
if /i "%SHA%"=="%GAME_SHA1_EN%" echo       default.xbe is the English-only European release.
if /i "%SHA%"=="%GAME_SHA1_EN%" exit /b 0
if /i "%SHA%"=="%GAME_SHA1_EN_XISO%" echo       default.xbe is the English-only European release, from an xiso copy.
if /i "%SHA%"=="%GAME_SHA1_EN_XISO%" exit /b 0
echo   assets\%GAME_XBE% is not from the European (PAL) release this port is made for.
echo     sha1 %SHA%
echo     expected one of the European releases:
echo       five-language disc %GAME_SHA1% or %GAME_SHA1_XISO%
echo       English-only disc  %GAME_SHA1_EN% or %GAME_SHA1_EN_XISO%
echo   Put the European (PAL) .iso or its extracted files in the assets folder instead.
exit /b 1

:nogame
echo   Put the full game files in the assets folder first:
echo     %CD%\assets
echo   It needs the European (PAL) .iso or %GAME_XBE% with the game's folders beside it.
echo   An .iso can also go beside build.bat.
goto :fail

:fail
echo.
echo   BUILD FAILED. Read the last message above.
echo.
if not defined CI pause
exit /b 1
